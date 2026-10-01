#include "imperivm/core/sim/infobar.hpp"

#include <algorithm>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/script/vm.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

std::span<const std::byte> bytes_of(std::string_view text) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

/// Comma-separated words, trimmed; the shape of `interface=` and
/// `unit_specials=`.
std::vector<std::string> split_words(std::string_view text) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find(',', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view word = text.substr(start, end - start);
    while (!word.empty() && (word.front() == ' ' || word.front() == '\t')) word.remove_prefix(1);
    while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) word.remove_suffix(1);
    if (!word.empty()) out.emplace_back(word);
    if (end == text.size()) break;
    start = end + 1;
  }
  return out;
}

bool same_fold(std::string_view a, std::string_view b) noexcept {
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

bool is_a(const World& world, const ClassGraph& graph, ObjectId id, std::string_view base) {
  const ClassIndex index = graph.find(base);
  return index != kNoClass && world.class_is_a(id, index);
}

/// The percentage `health / max_health`, or -1 when there is no health.
std::int32_t health_percent(const World& world, ObjectId id) {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return -1;
  std::int32_t max = 0;
  if (const CombatSystem* combat = combat_system_of(const_cast<World&>(world))) {
    max = combat->max_health(id);
  }
  if (max <= 0) return -1;
  const std::int32_t health = std::clamp(slot->state.health, 0, max);
  return health * 100 / max;
}

}  // namespace

// --------------------------------------------------------------------------
// which class
// --------------------------------------------------------------------------

std::string selection_class(const World& world, std::span<const ObjectId> selection) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return std::string();
  const auto class_of = [&](ObjectId id) -> ClassIndex {
    const WorldObject* slot = world.find(id);
    return slot == nullptr ? kNoClass : slot->class_index;
  };
  const auto exists = [&](const std::string& id) { return graph->find(id) != kNoClass; };

  std::vector<ObjectId> live;
  for (const ObjectId id : selection) {
    if (class_of(id) != kNoClass) live.push_back(id);
  }
  if (live.empty()) return "Empty";

  const ObjectId first = live.front();
  if (live.size() == 1) {
    if (is_a(world, *graph, first, "Wagon")) {
      // What the wagon carries: 0x005e5a4f tests `[obj+0x1d4]`, which is
      // `ObjectState::cargo_resource` -- zero is gold, one is food.
      const WorldObject* wagon = world.find(first);
      const bool food = wagon != nullptr &&
                        wagon->state.cargo_resource == static_cast<std::int32_t>(Resource::food);
      return food ? "WagonFood" : "WagonGold";
    }
    return std::string(graph->at(class_of(first)).id);
  }

  // Every one a peasant of one race: `<R>PeasantMulti`, when the class exists.
  {
    bool peasants = true;
    char race = 0;
    for (const ObjectId id : live) {
      if (!is_a(world, *graph, id, "Peasant")) {
        peasants = false;
        break;
      }
      const std::string_view class_id = graph->at(class_of(id)).id;
      const char letter = class_id.empty() ? 0 : class_id.front();
      if (race == 0) race = letter;
      if (letter != race) {
        peasants = false;
        break;
      }
    }
    if (peasants && race != 0) {
      const std::string candidate = std::string(1, race) + "PeasantMulti";
      if (exists(candidate)) return candidate;
    }
  }

  if (is_a(world, *graph, first, "Wagon")) return "Wagons";

  bool one_class = true;
  for (const ObjectId id : live) {
    if (class_of(id) != class_of(first)) {
      one_class = false;
      break;
    }
  }
  const bool ship = is_a(world, *graph, first, "ShipBattle");
  const bool multi = (!one_class && !is_a(world, *graph, first, "Sentry")) ||
                     is_a(world, *graph, first, "Hero");
  if (multi) return ship ? "MultiOneShip" : "Multi";
  if (ship) return "MultiOneShip";
  // "Has a projectile": the class's `projectile_class`, which is what the
  // string at unit+0x960 holds when it is not `**Invalid**`.
  const std::string_view projectile = graph->property(class_of(first), "projectile_class");
  return projectile.empty() ? "MultiOne" : "MultiOneRanged";
}

// --------------------------------------------------------------------------
// what the UIHolder strip lists
// --------------------------------------------------------------------------

std::vector<ObjectId> holder_list(World& world, std::span<const ObjectId> selection) {
  std::vector<ObjectId> out;
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return out;
  const HeroSystem* heroes = hero_system_of(world);
  const auto army_of = [&](ObjectId id) -> const std::vector<ObjectId>* {
    const HeroRecord* hero = heroes == nullptr ? nullptr : heroes->hero(id);
    return hero == nullptr ? nullptr : &hero->army;
  };
  const auto class_of = [&](ObjectId id) -> ClassIndex {
    const WorldObject* slot = world.find(id);
    return slot == nullptr ? kNoClass : slot->class_index;
  };

  std::vector<ObjectId> live;
  for (const ObjectId id : selection) {
    if (class_of(id) != kNoClass) live.push_back(id);
  }
  if (live.empty()) return out;

  if (live.size() == 1) {
    // A hero: its army, the hero not in it (0x006d2ca0 copies `[hero+0x1cc]`).
    if (const std::vector<ObjectId>* army = army_of(live.front())) {
      out = *army;
      return out;
    }
    // Anything else: its holder's contents, when it has a holder
    // (0x006d2ccf, `vtbl+0xe4`) -- here a settlement's garrison.
    if (EconomySystem* economy = economy_of(world)) {
      if (const Settlement* settlement = economy->settlements().for_object(live.front())) {
        out = settlement->holder.units;
      }
    }
    return out;
  }

  // Several: listed only for a `Multi` selection that is not all sentries
  // (0x006d2c2b..0x006d2c55); else the list is cleared.
  bool one_class = true;
  bool all_sentries = true;
  for (const ObjectId id : live) {
    if (class_of(id) != class_of(live.front())) one_class = false;
    if (!is_a(world, *graph, id, "Sentry")) all_sentries = false;
  }
  const bool hero_first = is_a(world, *graph, live.front(), "Hero");
  if ((one_class && !hero_first) || all_sentries) return out;
  out = live;
  for (const ObjectId id : live) {
    if (const std::vector<ObjectId>* army = army_of(id)) out.insert(out.end(), army->begin(), army->end());
  }
  return out;
}

// --------------------------------------------------------------------------
// the builder
// --------------------------------------------------------------------------

struct InfoBar::Impl {
  GameSession* session = nullptr;
  /// `SKILLS.INI` sections, in file order, which is the icon order: the file
  /// says so at its top. Each is the section name and its `icon`.
  std::vector<std::pair<std::string, std::string>> skills;
  std::vector<std::byte> skills_bytes;
  /// `UNIT_SPECIALS.INI`: section name -> (`icon`, `name`).
  std::vector<std::pair<std::string, std::pair<std::string, std::string>>> specials;
  std::vector<std::byte> specials_bytes;

  /// Compiled slot scripts by source text.
  struct Compiled {
    std::string source;
    script::Chunk chunk;
    bool ok = false;
    std::string complaint;
  };
  /// A deque, so a reference handed out survives the next compile.
  std::deque<Compiled> compiled;

  const Compiled& compile(std::string_view source) {
    for (const Compiled& entry : compiled) {
      if (entry.source == source) return entry;
    }
    Compiled entry;
    entry.source.assign(source);
    // An inline script has no signature comment and writes `.AsUnit.level`
    // with `this` declared nowhere; the original pre-binds it. Here `this`
    // is bound the way every `.vs` file binds its receiver -- as parameter
    // zero, through the signature the compiler already understands -- by
    // putting one in front of the source. The return type in it is recorded
    // and enforced by nothing, so `str` covers the slots that return a
    // number and the ones that return a string alike.
    const std::string wrapped = "// str, Obj this\n" + entry.source;
    script::Diagnostic diagnostic;
    const Result<script::Script> parsed =
        script::parse(bytes_of(wrapped), "<value>", &diagnostic);
    if (!parsed.ok()) {
      entry.complaint = "would not parse: " + entry.source;
    } else {
      script::CompileError error;
      const Result<script::Chunk> chunk =
          script::compile(*parsed, session->scheduler().registry(), &error);
      if (!chunk.ok()) {
        entry.complaint = "would not compile: " + entry.source + " (" + error.message + ")";
      } else {
        entry.chunk = *chunk;
        entry.ok = true;
      }
    }
    compiled.push_back(std::move(entry));
    return compiled.back();
  }

  /// Run one slot script with `this` bound to `receiver`, the way
  /// `ScriptOrderVerifier` runs a `verify=`: synchronously, off to the side,
  /// under no script record.
  bool evaluate(const Compiled& entry, ObjectId receiver, std::string& text,
                std::string& complaint) {
    script::Scheduler& scheduler = session->scheduler();
    HostContext& context = session->host_context();
    const std::array<script::Value, 1> args{
        receiver == kNoObject ? script::Value::object(script::ObjectRef{script::kNoType, 0})
                              : script::Value::object(context.object_type, receiver)};
    script::Execution execution = script::start(entry.chunk, args, scheduler.host());
    script::VmEnv env;
    env.registry = scheduler.registry();
    env.host = scheduler.host();
    env.scheduler = &scheduler;
    env.user = &context;
    env.script = script::kNoScript;
    env.now = scheduler.now();
    const script::ExecStatus status = script::run(execution, entry.chunk, env);
    if (status != script::ExecStatus::finished) {
      complaint = "trapped: " + entry.source;
      return false;
    }
    const script::Value& result = execution.result;
    if (result.is_nil()) {
      text.clear();
      return true;
    }
    if (result.is_integer()) {
      text = std::to_string(result.as_integer());
      return true;
    }
    if (result.is_string()) {
      text = result.as_string();
      return true;
    }
    // A handle: the host's spelling, which is what `pr` would print.
    script::Host* host = scheduler.host();
    if (host == nullptr) return false;
    const auto rendered = host->to_string(result);
    if (!rendered.ok()) return false;
    text = rendered.value();
    return true;
  }

  std::string translate(std::string_view key) const {
    const game::TranslationTable* table = session->host_context().translations;
    return std::string(table == nullptr ? key : table->translate(key));
  }
  /// A class's `display_name`, keyed in the table as `<name>@<Class> class
  /// name` (and `... class name plural`); 314 and 196 entries.
  std::string translate_class_name(std::string_view key, std::string_view class_id, bool plural) const {
    const game::TranslationTable* table = session->host_context().translations;
    if (table == nullptr) return std::string(key);
    std::string context(class_id);
    context += plural ? " class name plural" : " class name";
    return std::string(table->translate_in_context(key, context));
  }

  void load_skills(std::span<const std::byte> ini) {
    skills_bytes.assign(ini.begin(), ini.end());
    const Result<IniDocument> doc = IniDocument::parse(skills_bytes);
    if (!doc.ok()) return;
    for (SectionIndex i = 0; i < doc->sections().size(); ++i) {
      const std::string name(doc->sections()[i].name);
      skills.emplace_back(name, std::string(doc->value(i, "icon")));
    }
  }

  void load_specials(std::span<const std::byte> ini) {
    specials_bytes.assign(ini.begin(), ini.end());
    const Result<IniDocument> doc = IniDocument::parse(specials_bytes);
    if (!doc.ok()) return;
    for (SectionIndex i = 0; i < doc->sections().size(); ++i) {
      const std::string name(doc->sections()[i].name);
      specials.emplace_back(name, std::make_pair(std::string(doc->value(i, "icon")),
                                                  std::string(doc->value(i, "name"))));
    }
  }
};

InfoBar::InfoBar(GameSession& session, std::span<const std::byte> skills_ini,
                 std::span<const std::byte> specials_ini)
    : impl_(std::make_unique<Impl>()) {
  impl_->session = &session;
  impl_->load_skills(skills_ini);
  impl_->load_specials(specials_ini);
}

InfoBar::~InfoBar() = default;

SelectionInfo InfoBar::describe(PlayerId player) {
  SelectionInfo info;
  GameSession& session = *impl_->session;
  World& world = session.world();
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || player == kNoPlayer) return info;

  const std::span<const ObjectId> selection = session.selections().player(player).ids();
  info.selected = selection.size();
  info.class_id = selection_class(world, selection);
  const ClassIndex shown = graph->find(info.class_id);
  if (shown == kNoClass) return info;

  // The one object the declarations are about: the selection's first live
  // member, which is also what `this` binds to in a slot script.
  ObjectId subject = kNoObject;
  for (const ObjectId id : selection) {
    if (world.find(id) != nullptr) {
      subject = id;
      break;
    }
  }

  info.tags = split_words(graph->property(shown, "interface"));
  info.icon.assign(graph->property(shown, "icon"));
  if (subject != kNoObject && selection.size() == 1) {
    // The object's own `display_name` when the map gave it one (`Aeneas`,
    // `Grand priest`): the unit loader writes it over the class's into the
    // same string (`WorldObject::display_name`). Keyed bare in the table.
    const WorldObject* named = world.find(subject);
    if (named != nullptr && !named->display_name.empty()) {
      info.name = impl_->translate(named->display_name);
    } else {
      info.name = impl_->translate_class_name(graph->property(shown, "display_name"),
                                              graph->classes()[shown].id, false);
    }
    info.health = health_percent(world, subject);
  } else if (subject != kNoObject) {
    // A multiple selection is named by its plural, and its bar is the
    // `Multi` class's own value2 rather than a health bar.
    info.name = impl_->translate_class_name(graph->property(shown, "display_name_plural"),
                                            graph->classes()[shown].id, true);
  }

  // The six slots.
  const std::array<InfoValue, 6> values = graph->resolved_values(shown);
  for (std::size_t i = 0; i < values.size(); ++i) {
    const InfoValue& declared = values[i];
    if (declared.script.empty()) continue;
    const Impl::Compiled& compiled = impl_->compile(declared.script);
    InfoSlot& slot = info.values[i];
    slot.icon.assign(declared.icon);
    if (!compiled.ok) {
      info.complaints.push_back(compiled.complaint);
      continue;
    }
    std::string complaint;
    if (!impl_->evaluate(compiled, subject, slot.text, complaint)) {
      if (!complaint.empty()) info.complaints.push_back(std::move(complaint));
      continue;
    }
    slot.present = true;
  }
  if (subject == kNoObject) return info;

  // -- the strips ----------------------------------------------------------

  const WorldObject* slot = world.find(subject);
  const ClassIndex own = slot == nullptr ? kNoClass : slot->class_index;

  // The training queue: the building's `train` rows behind its running
  // command, by `queueicon`; the running one wears the train frame and the
  // bar `Obj::Progress` stamped, the rest the wait frame.
  if (CommandSystem* commands = command_system(world)) {
    if (const CommandQueue* queue = commands->find(subject)) {
      bool first = true;
      for (const Command& command : queue->entries) {
        const CommandDef* def = command.name.empty() ? nullptr : commands->table().find(command.name);
        if (def == nullptr || !def->train_command) continue;
        InfoCell cell;
        cell.icon = def->queue_icon;
        if (cell.icon.empty()) {
          // A row with no `queueicon` falls back to the trained class's portrait.
          const std::vector<std::string> fields = split_words(command.param);
          const ClassIndex trained = fields.empty() ? kNoClass : graph->find(fields.front());
          if (trained != kNoClass) cell.icon.assign(graph->property(trained, "icon"));
        }
        cell.frame = first ? InfoCell::Frame::train : InfoCell::Frame::wait;
        cell.command = command.id;
        if (first && &command == queue->running() && queue->progress_end > queue->progress_start) {
          const GameTime now = session.scheduler().now();
          const GameTime span = queue->progress_end - queue->progress_start;
          const GameTime done = std::clamp<GameTime>(now - queue->progress_start, 0, span);
          cell.health = static_cast<std::int32_t>(done * 100 / span);
        }
        first = false;
        info.queue.push_back(std::move(cell));
      }
    }
  }

  // The `UIHolder` strip: 0x006d2ad0's list, folded into cells by
  // 0x006d2d20's keys (see the header).
  {
    const HeroSystem* heroes = hero_system_of(world);
    const std::vector<ObjectId> listed = holder_list(world, selection);
    const bool one_hero = selection.size() == 1 && heroes != nullptr && heroes->hero(subject) != nullptr;
    constexpr std::uint64_t kHeroKey = 1ull << 62;
    constexpr std::uint64_t kSentryKey = (2ull << 62) | 0x1234ABCDu;
    constexpr std::uint64_t kClassKey = 3ull << 62;
    struct Group {
      std::uint64_t key = 0;
      ObjectId first = kNoObject;
      std::vector<ObjectId> picks;
      std::int64_t count = 0;
      std::int64_t health = 0;
      std::int64_t max = 0;
      bool hero_cell = false;  // a hero's cell: its number leaves the hero out
    };
    std::vector<Group> groups;
    const CombatSystem* combat = combat_system_of(world);
    for (const ObjectId id : listed) {
      const WorldObject* member = world.find(id);
      if (member == nullptr || member->class_index == kNoClass) continue;
      std::uint64_t key = 0;
      ObjectId first = id;
      bool pick = true;
      bool hero_cell = false;
      if (heroes != nullptr && heroes->hero(id) != nullptr) {
        key = kHeroKey | id;
        hero_cell = true;
      } else if (const ObjectId hero = heroes == nullptr ? kNoObject : heroes->hero_of(id);
                 !one_hero && hero != kNoObject && world.find(hero) != nullptr &&
                 std::find(listed.begin(), listed.end(), hero) != listed.end()) {
        // Folded into its hero's cell, which a click selects the hero of alone.
        key = kHeroKey | hero;
        first = hero;
        pick = false;
        hero_cell = true;
      } else if (is_a(world, *graph, id, "Sentry")) {
        key = kSentryKey;
      } else {
        key = kClassKey | (static_cast<std::uint64_t>(member->class_index) << 8) | member->state.owner;
      }
      auto found = std::find_if(groups.begin(), groups.end(), [key](const Group& g) { return g.key == key; });
      if (found == groups.end()) {
        groups.push_back(Group{});
        found = groups.end() - 1;
        found->key = key;
        found->first = first;
        found->hero_cell = hero_cell;
      }
      ++found->count;
      if (pick) found->picks.push_back(id);
      const std::int32_t max = combat == nullptr ? 0 : combat->max_health(id);
      if (max > 0) {
        found->health += std::clamp(member->state.health, 0, max);
        found->max += max;
      }
    }
    for (Group& group : groups) {
      const WorldObject* first = world.find(group.first);
      InfoCell cell;
      cell.key = group.key;
      cell.object = group.first;
      cell.objects = std::move(group.picks);
      if (first != nullptr && first->class_index != kNoClass) {
        cell.icon.assign(graph->property(first->class_index, "icon"));
      }
      const std::int64_t shown = group.hero_cell ? group.count - 1 : group.count;
      if (shown > 0) cell.number = std::to_string(shown);
      cell.health = group.max > 0 ? static_cast<std::int32_t>(group.health * 100 / group.max) : -1;
      info.holder.push_back(std::move(cell));
    }
  }

  // The items and the skills, both the hero system's.
  if (HeroSystem* heroes = hero_system_of(world)) {
    const ItemStore& store = heroes->items();
    const ItemCatalog* catalog = store.catalog();
    for (const ItemInstance& item : store.items()) {
      if (item.owner != subject) continue;
      InfoCell cell;
      cell.object = item.id;
      if (const ItemDefinition* def = catalog == nullptr ? nullptr : catalog->at(item.type)) {
        cell.icon = def->image;
        cell.text = impl_->translate(def->name);
        cell.glow = !def->script(ItemScriptHook::use).empty();
      }
      if (item.usecount > 0) cell.number = std::to_string(item.usecount);
      info.items.push_back(std::move(cell));
    }

    if (const HeroRecord* hero = heroes->hero(subject)) {
      for (std::size_t i = 0; i < impl_->skills.size() && i < kHeroSkillCount; ++i) {
        // `SKILLS.INI` order is `HeroSkill` order: the file says its order is
        // the icon order, and `sim/hero.hpp` numbers the enum from it.
        if (!hero->offered[i]) continue;
        InfoCell cell;
        cell.icon = impl_->skills[i].second;
        cell.text = impl_->translate(impl_->skills[i].first);
        if (hero->skills[i] > 0) cell.number = std::to_string(hero->skills[i]);
        cell.plus = heroes->available_skill_points(hero->id) > 0;
        info.skills.push_back(std::move(cell));
      }
    }
  }

  // The specials: the class's `unit_specials`, each a `UNIT_SPECIALS.INI`
  // section.
  if (own != kNoClass) {
    for (const std::string& special : split_words(graph->property(own, "unit_specials"))) {
      for (const auto& [name, entry] : impl_->specials) {
        if (!same_fold(name, special)) continue;
        InfoCell cell;
        cell.icon = entry.first;
        cell.text = impl_->translate(entry.second.empty() ? name : entry.second);
        info.specials.push_back(std::move(cell));
        break;
      }
    }
  }
  return info;
}

}  // namespace imperivm::core::sim
