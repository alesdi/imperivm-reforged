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

bool iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

/// Whether `def` is offered to the object: some `<src>` is an ancestor of its
/// class and no `<nsrc>` is.
bool offered_to(const World& world, const ClassGraph& graph, const CommandDef& def, ObjectId id) {
  bool offered = false;
  for (const std::string& source : def.sources) {
    const ClassIndex base = graph.find(source);
    if (base != kNoClass && world.class_is_a(id, base)) {
      offered = true;
      break;
    }
  }
  if (!offered) return false;
  for (const std::string& excluded : def.excludes) {
    const ClassIndex base = graph.find(excluded);
    if (base != kNoClass && world.class_is_a(id, base)) return false;
  }
  return true;
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

  [[nodiscard]] std::string translate(std::string_view key) const {
    const game::TranslationTable* table = session->host_context().translations;
    return std::string(table == nullptr ? key : table->translate(key));
  }

  /// The rows the selection offers, in button order, before any verifier.
  std::vector<const CommandDef*> offered(std::span<const ObjectId> selection) {
    World& world = session->world();
    const ClassGraph* graph = world.class_graph();
    CommandSystem* commands = command_system(world);
    std::vector<const CommandDef*> out;
    if (graph == nullptr || commands == nullptr) return out;
    std::vector<ObjectId> live;
    for (const ObjectId id : selection) {
      if (world.find(id) != nullptr) live.push_back(id);
    }
    if (live.empty()) return out;
    for (const CommandDef& def : commands->table().commands()) {
      if (def.sources.empty()) continue;
      // A row with no `button=` is a right-click verb -- `transport`,
      // `attack_unit_type`, `enter` -- and has no place on the bar.
      if (def.button.empty()) continue;
      bool all = true;
      for (const ObjectId id : live) {
        // The class offers the row *and* the object's `vtbl+0xd8` lets it:
        // 0x005e787b asks that predicate of every selected object inside the
        // same walk that tests the class's rows, and a row either test refuses
        // for any member is left off the bar -- not drawn unlit. So a building
        // that is not a ruin shows no Repair, a ruin shows nothing else, and a
        // row `CmdDisable` took away is gone.
        if (!offered_to(world, *graph, def, id) ||
            !commands->command_enabled(world, id, def.name)) {
          all = false;
          break;
        }
      }
      if (all) out.push_back(&def);
    }
    std::stable_sort(out.begin(), out.end(), [](const CommandDef* a, const CommandDef* b) {
      if (a->priority != b->priority) return a->priority < b->priority;
      return a->name < b->name;
    });
    return out;
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

  const CommandDef* find_offered(std::span<const ObjectId> selection, std::string_view name) {
    for (const CommandDef* def : offered(selection)) {
      if (iequal(def->name, name)) return def;
    }
    return nullptr;
  }

  /// `void f(ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier,
  /// int player)`: the row's own way of issuing itself, started on the
  /// scheduler like any script.
  bool dispatch(const CommandDef& def, std::span<const ObjectId> selection,
                const OrderTarget& target, bool replace, bool modifier, PlayerId player) {
    HostContext& context = session->host_context();
    script::Scheduler& scheduler = session->scheduler();
    if (context.library == nullptr) return false;
    const std::uint32_t chunk_index = context.library->chunk_for(def.group_dispatch);
    if (chunk_index == script::kNoChunk) return false;
    World& world = session->world();
    ObjListPool& pool = objlist_pool_of(world);
    // The list belongs to the script that is about to run: acquired under
    // `kNoScript` and handed over as an argument, it is released with the
    // script's own temporaries only if the script takes it; a dispatch that
    // never runs leaves it in the `kNoScript` slot, which the next verifier
    // clears.
    const ObjListId list = pool.acquire_temporary(script::kNoScript);
    if (std::vector<ObjectId>* items = pool.mutable_items(list)) {
      items->assign(selection.begin(), selection.end());
    }
    const std::array<script::Value, 6> args{
        make_objlist_value(list),
        pack_point(target.point),
        target.is_object() ? script::Value::object(context.object_type, target.object)
                           : script::Value::object(script::ObjectRef{script::kNoType, 0}),
        script::Value::boolean(replace),
        script::Value::boolean(modifier),
        script::Value::integer(static_cast<std::int32_t>(player) + 1),
    };
    return scheduler.spawn(chunk_index, args, script::ObjectRef{}) != script::kNoScript;
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
  const std::span<const ObjectId> selection = impl_->session->selections().player(player).ids();
  for (const CommandDef* def : impl_->offered(selection)) {
    if (out.size() >= impl_->max_buttons) break;
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
    std::string reason;
    button.enabled = impl_->verify(*def, selection, reason);
    if (!reason.empty()) button.reason = impl_->translate(reason);
    out.push_back(std::move(button));
  }
  return out;
}

CommandBar::Flags CommandBar::flags(PlayerId player, std::string_view name, Keys keys) {
  const std::span<const ObjectId> live = impl_->session->selections().player(player).ids();
  const std::vector<ObjectId> selection(live.begin(), live.end());
  const CommandDef* def = impl_->find_offered(selection, name);
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
  const std::span<const ObjectId> live = impl_->session->selections().player(player).ids();
  const std::vector<ObjectId> selection(live.begin(), live.end());
  const CommandDef* def = impl_->find_offered(selection, name);
  if (def == nullptr) return Press::unknown;
  std::string reason;
  if (!impl_->verify(*def, selection, reason)) return Press::disabled;
  return def->needs_target() ? Press::waiting : Press::issued;
}

CommandBar::Press CommandBar::press(PlayerId player, std::string_view name,
                                    std::span<const ObjectId> selection, Flags flags) {
  const CommandDef* def = impl_->find_offered(selection, name);
  if (def == nullptr) return Press::unknown;
  std::string reason;
  if (!impl_->verify(*def, selection, reason)) return Press::disabled;
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

bool CommandBar::aim(PlayerId player, std::string_view name, std::span<const ObjectId> selection,
                     const OrderTarget& target, Flags flags) {
  const CommandDef* def = impl_->find_offered(selection, name);
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
