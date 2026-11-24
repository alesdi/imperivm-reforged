#include "imperivm/core/sim/cmdbar.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/script/vm.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

/// The selection insert's ownership test (0x005b92b0): the owner's relation
/// towards `player` carries share-control (bit 5). An unowned object is
/// nobody's.
bool owned_by(const World& world, const WorldObject& object, PlayerId player) noexcept {
  const PlayerId owner = object.state.owner;
  if (!PlayerTable::is_valid(owner) || !PlayerTable::is_valid(player)) return false;
  return world.players().has(owner, player, Relation::share_control);
}

/// `button=` is under the bar's `%Buttons%`, which every shipped command bar
/// sets to `gameres/CmdBar`; the interface's alias table turns that into the
/// pack path.
std::string button_path(const CommandDef& def) {
  if (def.button.empty()) return std::string();
  std::string path = "gameres/CmdBar/";
  path += def.button;
  return path;
}

}  // namespace

struct CommandBar::Impl {
  GameSession* session = nullptr;
  std::size_t max_buttons = 16;
  /// The one-time rows posted and not yet executed, by id: the interface's
  /// set at `+0x5c`. The local interface's, not world state.
  std::vector<std::uint32_t> in_flight;

  [[nodiscard]] std::string translate(std::string_view key) const {
    const game::TranslationTable* table = session->host_context().translations;
    return std::string(table == nullptr ? key : table->translate(key));
  }

  /// The selection as the bar holds it: the members that resolve to a
  /// classed object, in the selection's order.
  std::vector<ObjectId> members(std::span<const ObjectId> selection) const {
    World& world = session->world();
    std::vector<ObjectId> live;
    for (const ObjectId id : selection) {
      const WorldObject* slot = world.find(id);
      if (slot != nullptr && slot->class_index != kNoClass) live.push_back(id);
    }
    return live;
  }

  /// The bar's list (0x005e7710): the rows the selection offers, in button
  /// order, before the builder's own tests. See `cmdbar.hpp` for the reading.
  std::vector<const CommandDef*> rows(std::span<const ObjectId> live, PlayerId player) {
    std::vector<const CommandDef*> out;
    World& world = session->world();
    const ClassGraph* graph = world.class_graph();
    CommandSystem* commands = command_system(world);
    if (graph == nullptr || commands == nullptr || live.empty()) return out;
    // The gate (0x005b8ae0): a selection the player controls -- the insert
    // keeps a foreign object only alone, so one own member is the whole
    // selection's control -- or a foreign building alone whose class is a
    // `Stonehenge` (selection type 4 and 0x0059c020 on its class).
    bool control = false;
    for (const ObjectId id : live) {
      if (owned_by(world, *world.find(id), player)) control = true;
    }
    if (!control) {
      const WorldObject* only = live.size() == 1 ? world.find(live.front()) : nullptr;
      const ClassIndex stonehenge = graph->lookup("Stonehenge");
      if (only == nullptr || !only->state.flags.is_building || stonehenge == kNoClass ||
          !world.class_is_a(only->id, stonehenge)) {
        return out;
      }
    }
    // The selection's classes (`[sel+0x20]`): each member's, the first time
    // it is met, in the members' order.
    std::vector<ClassIndex> classes;
    for (const ObjectId id : live) {
      const ClassIndex cls = world.find(id)->class_index;
      if (std::find(classes.begin(), classes.end(), cls) == classes.end()) classes.push_back(cls);
    }
    const CommandClassIndex& index = commands->class_index(*graph);
    const CommandTable& table = commands->table();
    std::vector<std::uint32_t> list;
    // The sticky rows: for each class, its sticky list in order, a row not
    // already on the bar's list taken when some selected object of exactly
    // that class is let have it (0x005e38d0: `[obj+0x3c]` equal, then the
    // object's `vtbl+0xd8`).
    for (const ClassIndex cls : classes) {
      for (const std::uint32_t id : index.sticky(cls)) {
        if (std::find(list.begin(), list.end(), id) != list.end()) continue;
        const CommandDef* def = table.at(id);
        if (def == nullptr) continue;
        for (const ObjectId member : live) {
          if (world.find(member)->class_index == cls && commands->command_enabled(world, member, *def)) {
            list.push_back(id);
            break;
          }
        }
      }
    }
    // The offered rows: the first class's offered list in order, a row taken
    // when every selected object's class offers it too and every object is
    // let have it (0x005e7834..0x005e78c8). Not checked against the sticky
    // rows already taken: a row both sticky and offered is on the list twice.
    for (const std::uint32_t id : index.offered(classes.front())) {
      const CommandDef* def = table.at(id);
      if (def == nullptr) continue;
      bool all = true;
      for (const ObjectId member : live) {
        if (!index.offers(world.find(member)->class_index, id) ||
            !commands->command_enabled(world, member, *def)) {
          all = false;
          break;
        }
      }
      if (all) list.push_back(id);
    }
    out.reserve(list.size());
    for (const std::uint32_t id : list) out.push_back(table.at(id));
    // `std::list::sort` (0x005e7250, merging through 0x005e5b60): stable, by
    // `priority` (`[def+0x1c0]`) ascending, so ties keep the list's order.
    std::stable_sort(out.begin(), out.end(), [](const CommandDef* a, const CommandDef* b) {
      return a->priority < b->priority;
    });
    return out;
  }

  /// The builder's second test (0x006b3050 -> 0x005e20c0), asked of a
  /// one-time row only (`[def+0]`): refused while the row is in flight (the
  /// interface's set at `+0x5c`), and a `plrresearch` row refused while the
  /// player of the selection's first object, when that is a building, holds
  /// it. A refusal says `" "`, which `describe` translates.
  bool held_back(const CommandDef& def, std::span<const ObjectId> live, std::string& reason) const {
    if (!def.one_time) return false;
    bool refused = std::find(in_flight.begin(), in_flight.end(), def.id) != in_flight.end();
    if (!refused && def.plr_research && !live.empty()) {
      World& world = session->world();
      const WorldObject* first = world.find(live.front());
      const CommandSystem* commands = command_system(world);
      if (first != nullptr && first->state.flags.is_building && commands != nullptr) {
        refused = commands->player_holds_row(world, first->state.owner, def);
      }
    }
    if (refused) reason = " ";
    return refused;
  }

  /// The builder's verdict on one row (0x006b3190): the group verifier when
  /// the row has one, and then, when it passed or there is none, `held_back`.
  /// True when the row is to be lit.
  bool judge(const CommandDef& def, std::span<const ObjectId> live, std::string& reason) {
    if (!verify(def, live, reason)) return false;
    return !held_back(def, live, reason);
  }

  /// Run `bool f(ObjList objs, str OUT reasonText)` over the selection, with
  /// the row described so that `cmdparam` and `cmdcost_*` answer for it.
  /// Returns true when the verifier passed or could not be run: a button
  /// that cannot be judged stays pressable, and the press itself is judged
  /// again.
  bool verify(const CommandDef& def, std::span<const ObjectId> selection, std::string& reason) {
    reason.clear();
    if (def.group_verifier.empty()) return true;
    HostContext& context = session->host_context();
    script::Scheduler& scheduler = session->scheduler();
    if (context.library == nullptr) return true;
    const std::uint32_t chunk_index = context.library->chunk_for(def.group_verifier);
    if (chunk_index == script::kNoChunk || chunk_index >= scheduler.chunk_count()) return true;
    const script::Chunk& chunk = scheduler.chunk(chunk_index);

    World& world = session->world();
    ObjListPool& pool = objlist_pool_of(world);
    const ObjListId list = pool.acquire_temporary(script::kNoScript);
    if (std::vector<ObjectId>* items = pool.mutable_items(list)) {
      items->assign(selection.begin(), selection.end());
    }
    const std::array<script::Value, 2> args{make_objlist_value(list),
                                            script::Value::string(std::string())};
    const std::string_view described = context.described_command;
    context.described_command = def.name;
    script::Execution execution = script::start(chunk, args, scheduler.host());
    script::VmEnv env;
    env.registry = scheduler.registry();
    env.host = scheduler.host();
    env.scheduler = &scheduler;
    env.user = &context;
    env.script = script::kNoScript;
    env.now = scheduler.now();
    const script::ExecStatus status = script::run(execution, chunk, env);
    context.described_command = described;
    // The temporary list is `kNoScript`'s; a verifier that built lists of
    // its own left them in the same slot, and they go with it.
    pool.release_script(script::kNoScript);
    if (status != script::ExecStatus::finished) return true;
    const bool passed = execution.result.truthy_scalar();
    if (!passed && !execution.frames.empty() && execution.frames.front().locals.size() > 1) {
      const script::Value& text = execution.frames.front().locals[1];
      if (text.is_string()) reason = text.as_string();
    }
    return passed;
  }

  /// Whether a press of `def` replaces what the actors are running.
  ///
  /// `gbr.exe` 0x005e39f0 sets the flags of every order the bar posts -- a
  /// press issued at once (called from 0x005e6bd0) and an aimed one alike
  /// (0x005e720e): **a `traincommand="yes"` row (`[def+0x1b4]`) never
  /// replaces**, whatever the keys say; every other row replaces unless Shift
  /// is held. So a second unit's button on a barracks queues behind the one
  /// in training (playtest #14) and a plain move still overrides the last.
  /// The flag is per command, read from the row, and not per object or per
  /// method: a research press (`researchcommand`, `[def+0x1b8]`, not read
  /// here) on a barracks replaces like a move, cancelling what it trains.
  ///
  /// Shift decides this and nothing else: `bModifier` is Ctrl, read by the
  /// same routine into the order's `+0x10` (`Keys`).
  static bool replaces(const CommandDef& def, bool append) noexcept {
    return !append && !def.train_command;
  }

  /// What `keys` post for `def`: Shift, Ctrl, and -- Ctrl on a train row --
  /// the `TrainMultipleCount` the bar reads at the click (0x005e3a91, the
  /// `GamePlay` table; the out value starts at zero, so a missing key is 0),
  /// kept as the byte the order stores it in.
  Flags flags_for(const CommandDef& def, Keys keys) {
    Flags out;
    out.append = keys.shift;
    out.modifier = keys.ctrl;
    if (keys.ctrl && def.train_command) {
      std::int32_t count = 0;
      if (const EnvSystem* env = env_of(session->world())) (void)env->constant("TrainMultipleCount", count);
      out.repeat = static_cast<std::uint8_t>(count);
    }
    return out;
  }

  /// The row `name` among the ones the bar would draw a button for. A press
  /// is of a button, which holds its row; this engine names it.
  const CommandDef* find_offered(std::span<const ObjectId> live, PlayerId player,
                                 std::string_view name) {
    for (const CommandDef* def : rows(live, player)) {
      if (!def->button.empty() && def->name == name) return def;
    }
    return nullptr;
  }

  /// `void f(ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier,
  /// int player)`: the row's own way of issuing itself, **run to its end
  /// inside the press**.
  ///
  /// `gbr.exe`'s per-group issue (0x004ef3d0, from the row's execution
  /// 0x004f1610) builds an `ObjList` of the actors, sets up an interpreter
  /// context for the row's compiled dispatch (`[def+0x168]`, 0x0069d090 and
  /// 0x0069d4a0), runs it with the synchronous budget `0x0fffffff`
  /// (0x0069d4c0) and destroys the context (0x0069d390) before it returns.
  /// So a dispatch is not a peer that runs on some later pass: what it does is
  /// done when the press is, which is `Scheduler::call`. A dispatch that
  /// sleeps would be abandoned where it stands, as `call` says of any
  /// synchronous run (an inference there); none of the eleven shipped
  /// `groupdispatch=` scripts sleeps, waits or loops on one.
  ///
  /// It used to be spawned, and its list was acquired under `kNoScript` and
  /// left there for the script to take on its first pass. The app describes
  /// the bar again after every press, and each verifier it runs releases the
  /// `kNoScript` lists -- so on the way to that pass the list was freed, and
  /// the dispatch found `objs` empty and did nothing (playtest #21:
  /// `GROUP_DETACH.VS` never reached `DetachFrom`). Run inside the press, the
  /// list lives exactly as long as the call and is released after it.
  bool dispatch(const CommandDef& def, std::span<const ObjectId> selection,
                const OrderTarget& target, bool replace, bool modifier, PlayerId player) {
    // The issue (`CommandSystem::issue`): the commanded flag on every unit,
    // then the dispatch arm with the row's `cmdparam` and `cmdcost_*` ambient
    // for the call (`CommandSystem::dispatch`) -- `ACTIVATE_HERO_SKILL.VS`
    // reads both, and used to trap on the first.
    World& world = session->world();
    CommandSystem* commands = command_system(world);
    if (commands == nullptr) return false;
    return commands->issue(world, selection, def, target.point,
                           target.is_object() ? target.object : kNoObject, replace, modifier,
                           static_cast<std::int32_t>(player) + 1, /*user=*/true) != 0;
  }
};

CommandBar::CommandBar(GameSession& session) : impl_(std::make_unique<Impl>()) {
  impl_->session = &session;
}

CommandBar::~CommandBar() = default;

void CommandBar::set_max_buttons(std::size_t count) noexcept { impl_->max_buttons = count; }
std::size_t CommandBar::max_buttons() const noexcept { return impl_->max_buttons; }

std::vector<CommandButton> CommandBar::describe(PlayerId player) {
  std::vector<CommandButton> out;
  if (player == kNoPlayer) return out;
  const std::vector<ObjectId> live =
      impl_->members(impl_->session->selections().player(player).ids());
  // The builder (0x006b3d60): the list in order, a slot per drawn row until
  // the slots run out. A row with no button image (`[def+0x174]`, loaded
  // from `button=`) is passed over; so is a row its verdict refuses without
  // a word.
  for (const CommandDef* def : impl_->rows(live, player)) {
    if (out.size() >= impl_->max_buttons) break;
    if (def->button.empty()) continue;
    std::string reason;
    const bool lit = impl_->judge(*def, live, reason);
    if (!lit && reason.empty()) continue;
    CommandButton button;
    button.name = def->name;
    button.icon = button_path(*def);
    button.rollover = def->rollover;
    button.description = def->description;
    button.key = def->key;
    button.cursor = def->cursor;
    button.needs_target = def->needs_target();
    button.priority = def->priority;
    button.cost_gold = def->cost_gold;
    button.cost_food = def->cost_food;
    button.cost_pop = def->cost_pop;
    button.cost_stamina = def->cost_stamina;
    button.enabled = lit;
    if (!reason.empty()) button.reason = impl_->translate(reason);
    out.push_back(std::move(button));
  }
  return out;
}

CommandBar::Flags CommandBar::flags(PlayerId player, std::string_view name, Keys keys) {
  const std::vector<ObjectId> live =
      impl_->members(impl_->session->selections().player(player).ids());
  const CommandDef* def = impl_->find_offered(live, player, name);
  if (def == nullptr) return Flags{keys.shift, keys.ctrl, 1};
  return impl_->flags_for(*def, keys);
}

CommandBar::Press CommandBar::press(PlayerId player, std::string_view name, Keys keys) {
  // A copy: pressing runs scripts, and a script may change the selection.
  const std::span<const ObjectId> live = impl_->session->selections().player(player).ids();
  const std::vector<ObjectId> selection(live.begin(), live.end());
  return press(player, name, selection, flags(player, name, keys));
}

CommandBar::Press CommandBar::check(PlayerId player, std::string_view name) {
  const std::vector<ObjectId> live =
      impl_->members(impl_->session->selections().player(player).ids());
  const CommandDef* def = impl_->find_offered(live, player, name);
  if (def == nullptr) return Press::unknown;
  std::string reason;
  if (!impl_->judge(*def, live, reason)) return Press::disabled;
  return def->needs_target() ? Press::waiting : Press::issued;
}

bool CommandBar::post(std::string_view name) {
  const CommandSystem* commands = command_system(impl_->session->world());
  const CommandDef* def = commands == nullptr ? nullptr : commands->table().find(name);
  if (def == nullptr || !def->one_time) return true;
  std::vector<std::uint32_t>& held = impl_->in_flight;
  if (std::find(held.begin(), held.end(), def->id) != held.end()) return false;
  held.push_back(def->id);
  return true;
}

void CommandBar::executed(std::string_view name) {
  const CommandSystem* commands = command_system(impl_->session->world());
  const CommandDef* def = commands == nullptr ? nullptr : commands->table().find(name);
  if (def == nullptr) return;
  std::vector<std::uint32_t>& held = impl_->in_flight;
  held.erase(std::remove(held.begin(), held.end(), def->id), held.end());
}

CommandBar::Press CommandBar::press(PlayerId player, std::string_view name,
                                    std::span<const ObjectId> actors, Flags flags) {
  const std::vector<ObjectId> selection = impl_->members(actors);
  const CommandDef* def = impl_->find_offered(selection, player, name);
  if (def == nullptr) return Press::unknown;
  std::string reason;
  if (!impl_->judge(*def, selection, reason)) return Press::disabled;
  if (def->needs_target()) return Press::waiting;
  OrderTarget nowhere;
  const bool replace = Impl::replaces(*def, flags.append);
  if (!def->group_dispatch.empty()) {
    // Each repeat is the whole execution again (0x004e5e60 loops 0x004f1610),
    // so a repeated dispatch row starts its script that many times.
    bool started = flags.repeat == 0;
    for (std::uint8_t r = 0; r < flags.repeat; ++r) {
      started = impl_->dispatch(*def, selection, nowhere, replace, flags.modifier, player) || started;
    }
    return started ? Press::issued : Press::unknown;
  }
  // Shift appends, as a right click's does; a train row always does. The
  // per-object issue masks a train row again (0x004efbbb, `issue_order`), so
  // for the rows here the bar's own mask is an equivalence, kept because it is
  // the original's and the dispatch above needs it.
  const OrderMode mode = replace ? OrderMode::replace : OrderMode::append;
  for (std::uint8_t r = 0; r < flags.repeat; ++r) {
    for (const ObjectId actor : selection) {
      (void)issue_order(impl_->session->world(), actor, *def, nowhere, mode, /*user=*/true);
    }
  }
  return Press::issued;
}

bool CommandBar::aim(PlayerId player, std::string_view name, const OrderTarget& target,
                     Keys keys) {
  const std::span<const ObjectId> live = impl_->session->selections().player(player).ids();
  const std::vector<ObjectId> selection(live.begin(), live.end());
  return aim(player, name, selection, target, flags(player, name, keys));
}

bool CommandBar::aim(PlayerId player, std::string_view name, std::span<const ObjectId> actors,
                     const OrderTarget& target, Flags flags) {
  const std::vector<ObjectId> selection = impl_->members(actors);
  const CommandDef* def = impl_->find_offered(selection, player, name);
  if (def == nullptr) return false;
  const bool replace = Impl::replaces(*def, flags.append);
  // An aimed order goes through the same 0x005e39f0 (from 0x005e720e), so a
  // Ctrl aim of a train row -- the catapult's `attack` -- repeats too.
  if (!def->group_dispatch.empty()) {
    bool started = flags.repeat == 0;
    for (std::uint8_t r = 0; r < flags.repeat; ++r) {
      started = impl_->dispatch(*def, selection, target, replace, flags.modifier, player) || started;
    }
    return started;
  }
  const OrderMode mode = replace ? OrderMode::replace : OrderMode::append;
  for (std::uint8_t r = 0; r < flags.repeat; ++r) {
    for (const ObjectId actor : selection) {
      (void)issue_order(impl_->session->world(), actor, *def, target, mode, /*user=*/true);
    }
  }
  return true;
}

bool CommandBarSink::command(const NetOrder& order) {
  if (bar_ == nullptr) return false;
  // The row's execution lowers its in-flight mark first, whoever posted it
  // (0x004f1610 -> 0x005e14d0).
  bar_->executed(order.command);
  std::vector<ObjectId> actors;
  actors.reserve(order.actors.size());
  for (const ObjectId actor : order.actors) {
    if (is_commandable(session_->world(), actor, order.issuer)) actors.push_back(actor);
  }
  if (actors.empty()) return false;
  // Shift travels as the order's mode and Ctrl as its modifier, the two
  // flags 0x005e39f0 writes and the serialiser packs (0x004e7a27).
  CommandBar::Flags flags;
  flags.append = order.mode == OrderMode::append;
  flags.modifier = order.modifier;
  flags.repeat = order.repeat;
  if (order.aimed) return bar_->aim(order.issuer, order.command, actors, order.target, flags);
  return bar_->press(order.issuer, order.command, actors, flags) == CommandBar::Press::issued;
}

bool CommandBarSink::surrender(PlayerId issuer) {
  if (!PlayerTable::is_valid(issuer)) return false;
  session_->declare_match(issuer, /*lost=*/true);
  return true;
}

bool CommandBarSink::take_over(PlayerId departed) {
  if (!PlayerTable::is_valid(departed)) return false;
  World& world = session_->world();
  const PlayerSetup& setup = world.players().setup(departed);
  return ai_start(world, departed, setup.ai_script, ai_difficulty_overlay(setup.difficulty),
                  session_->scheduler()) == AiStartStatus::ok;
}

bool CommandBarSink::hand_back(PlayerId joined) {
  if (!PlayerTable::is_valid(joined)) return false;
  return ai_stop(session_->world(), joined, session_->scheduler());
}

}  // namespace imperivm::core::sim
