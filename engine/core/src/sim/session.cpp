// One running game.
// See include/imperivm/core/sim/session.hpp.

#include "imperivm/core/sim/session.hpp"

#include "imperivm/core/sim/fog.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/hooks.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/world/editor.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::core::sim {
namespace {

/// A whole decimal, as the original's serialiser writes every attribute; a
/// partial parse is refused rather than read short.
bool parse_authored_int(std::string_view text, std::int32_t& out) noexcept {
  if (text.empty()) return false;
  std::size_t i = 0;
  const bool negative = text[0] == '-';
  if (text[0] == '-' || text[0] == '+') i = 1;
  if (i >= text.size()) return false;
  std::int64_t value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
    if (value > 0x7FFFFFFF) return false;
  }
  out = static_cast<std::int32_t>(negative ? -value : value);
  return true;
}

/// The method a class binds for "do your own thing".
///
/// `GUARD.VS` tests `o.command == "idle"` as a unit's resting state and every
/// unit class binds `<method sig="idle">`, so this is the entry point that
/// makes a loaded object a running one.
constexpr std::string_view kIdleMethod = "idle";

/// `"ISES"`, little-endian: the session section's own magic, beside the
/// systems' in `save_systems.cpp`.
constexpr std::uint32_t kSessionMagic = 0x53455349u;
/// The session section's layout version. 1: magic, version, script watermark.
/// 2: the passability layer follows the watermark, when it was rebuilt.
/// 3: the templates laid -- index and origin each -- follow the layer, so a
///    load lays their ground again.
/// 4: the AI's nodes follow the templates -- centre, area and settlement each
///    -- because a node outlives the town hall it was centred on and a
///    rebuild cannot see where a razed one stood. See `GaikaTable::restore`.
constexpr std::uint32_t kSessionSectionVersion = 4;
/// One saved node: centre x and y, area, settlement.
constexpr std::size_t kSavedGaikaNodeBytes = 16;

}  // namespace

/// Everything the session owns.
///
/// Member order is load bearing: `world_` is constructed before the systems
/// that will be registered on it and before `host_`, which holds a reference to
/// it. The systems are members rather than allocations because `World` does not
/// own them -- deliberately, so that four domains' allocation does not end up
/// in one vector -- which makes their lifetime this class's problem.
struct GameSession::Impl : ScriptLibrary, ClassHookRunner {
  const script::HostRegistry* registry = nullptr;

  World world;

  MovementSystem movement;
  CombatSystem combat;
  EconomySystem economy;
  FeederSystem feeder;
  HeroSystem hero;
  EnvSystem env;
  CommandSystem command;
  MatchSystem match;
  AiSystem ai;
  CampaignSystem campaign;
  AreaSystem areas;
  FogSystem fog;

  AiProfile profile;
  game::TranslationTable translations;
  /// `SessionInputs::items`, parsed once; the hero system's store points at it.
  ItemCatalog items;
  /// `SessionInputs::settlement_templates`, kept until `start_match` reads it.
  std::vector<std::byte> settlement_templates;
  MaterialiseReport materialised;
  /// What the loader made of the map's objects; the editor's join back.
  World::PopulateReport populated;

  WorldHost host{world};
  HostContext context;
  script::Scheduler scheduler;
  SelectionTable selections;

  ScriptResolver* scripts = nullptr;
  /// `game.xml`, kept because the match rules are parsed from it on demand.
  std::vector<std::byte> game_properties;
  std::vector<std::byte> map_properties;
  std::vector<std::byte> terrain;
  std::vector<std::byte> height;
  /// The passability rebuild's inputs (`SessionInputs::masks`), kept the
  /// same way; `decor_masks` and `class_masks` fill on the first rebuild.
  std::vector<std::byte> decor;
  /// The decoration layer over `decor`, writable, for the templates'
  /// bulldozer; empty when the map has none.
  Grid decor_grid;
  std::vector<std::byte> terrain_table;
  std::vector<std::byte> decor_table;
  PassMaskResolver* masks = nullptr;
  /// `SessionInputs::settlement_template_terrain`, kept: a load lays the
  /// templates' ground again from the stamps the save carries.
  std::vector<std::byte> settlement_template_terrain;
  GroundReport ground;
  /// The templates' ground laid for `stamps`, in order; see
  /// `stamp_template_ground`. The layers must be the writable ones.
  void lay_template_ground(std::span<const TemplateStamp> stamps);
  Season season = Season::base;
  std::vector<const edit::PassMask*> decor_masks;
  std::map<ClassIndex, const edit::PassMask*> class_masks;
  /// The layer the grid is built from: the map's copy at `create`, rebuilt
  /// at `start_match`, carried by the save. Empty when the map has none.
  OwnedGrid pass_layer;
  PassabilityReport passability;
  /// See the definition beside `start_match`.
  bool rebuild_passability(const edit::WorldRect& rect, PassabilityReport& report);
  const ClassGraph* classes = nullptr;
  /// Kept from `SessionInputs` because `load` needs it: `World::deserialize`
  /// re-resolves every object's art through it, exactly as `populate_from_map`
  /// resolved it the first time.
  EntityResolver* entities = nullptr;

  /// Chunk index per `.vs` path, so a script is compiled once however many
  /// classes bind it. Ordered, not hashed: the compile order decides chunk
  /// indices, and a serialised coroutine names its code by one.
  std::map<std::string, std::uint32_t, std::less<>> chunks;
  /// Paths that failed to parse or compile, so the report can say so once
  /// rather than per object.
  std::map<std::string, std::string, std::less<>> broken;

  std::uint64_t turns = 0;
  std::size_t started = 0;
  /// The first object id `start_object_scripts` has not considered yet.
  ///
  /// Object ids are monotonic and never reused, so a watermark is an exact
  /// record of "everything below this has had its chance" -- no per-object
  /// bookkeeping, and the same answer on every peer. It is what lets the sweep
  /// run every turn instead of only at load, which is what a spawned object
  /// needs: `SpawnGroup` mints objects mid-game and they have to start running
  /// their class's `idle` method like anything else.
  ObjectId script_watermark = kNoObject;
  /// Whether the embedder has asked for object scripts at all.
  ///
  /// `start_object_scripts` is opt-in -- `GameSession::create` does not call
  /// it, and a conformance run drives the systems with no scripts on purpose --
  /// so the per-turn sweep has to inherit that choice rather than override it.
  /// A session that never asked keeps not running them, spawned or not.
  bool object_scripts_wanted = false;
  std::size_t finished = 0;

  /// Distinct trap message to (count, first script). Ordered so that two runs
  /// of one session produce byte-identical reports.
  struct TrapTally {
    std::size_t scripts = 0;
    script::ScriptId first = script::kNoScript;
  };
  std::map<std::string, TrapTally, std::less<>> traps;

  /// Compile `path` if it is not already in the library. Returns the chunk
  /// index, or `kNoChunk` when there is no source or it does not build.
  ///
  /// Also the whole of `ScriptLibrary`, which is how a host function reaches
  /// it: `RunAIHelper` names a file no manifest lists, so it has to be able to
  /// ask for one by path. See `sim/host_context.hpp`.
  std::uint32_t chunk_for(std::string_view path) override;

  /// And the whole of `ClassHookRunner`, for the same reason one layer down:
  /// `CombatSystem::advance` has to start a script and holds no scheduler.
  /// This is the one place with the class graph, the chunk cache and the
  /// scheduler at once, which is what makes it the implementation rather than
  /// the seam. See `sim/hooks.hpp`.
  bool run_class_hook(ClassHook hook, ObjectId subject, ObjectId argument) override;

  void collect_traps(const script::RunReport& report);
  void seed_economy(const MapObjectList* map, const World::PopulateReport* loaded);
  /// `Level`, `hs*` and `slot*` onto the hero system, once it has started.
  void apply_authored_attributes(const MapObjectList& map, std::span<const ObjectId> object_ids);
  /// Compile every `.vs` a restored session needs. See `load`.
  void prime_library(std::span<const std::byte> script_section);
};

void GameSession::Impl::seed_economy(const MapObjectList* map,
                                     const World::PopulateReport* loaded) {
  if (classes == nullptr) return;

  // `<settlement name>` by settlement object, from the loader's own join. The
  // reader has always parsed the attribute (`world/map.cpp`) and the world has
  // always dropped it, so every `WaitSettlementCapture` and `GetSettlement`
  // call in the installation had nothing to resolve against.
  const auto declared_of = [&](ObjectId settlement_object) -> const MapSettlement* {
    if (map == nullptr || loaded == nullptr) return nullptr;
    const std::span<const MapSettlement> declared = map->settlements();
    const std::size_t count =
        std::min(declared.size(), loaded->settlement_ids.size());
    for (std::size_t i = 0; i < count; ++i) {
      if (loaded->settlement_ids[i] == settlement_object) return &declared[i];
    }
    return nullptr;
  };

  // The triple is contiguous -- settlement, holder, warehouse at three
  // consecutive ids, in 328 of 328 cases across all nine dumps -- and
  // `World::spawn_settlement` reproduces that, so the two companions are found
  // by arithmetic rather than by search.
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::settlement) continue;

    SettlementInit init;
    init.settlement_object = slot.id;
    init.holder_object = slot.id + 1;
    init.warehouse_object = slot.id + 2;
    init.owner = slot.state.owner;
    const MapSettlement* declared = declared_of(slot.id);
    if (declared != nullptr) init.name = declared->name;

    // The anchor is the first object the loader linked back to this
    // settlement, in ascending id order -- which is map document order, so it
    // is the settlement's first building, and what `GetCentralBuilding`
    // returns.
    for (const WorldObject& candidate : world.objects()) {
      if (candidate.internal != InternalKind::none) continue;
      if (candidate.settlement != slot.id) continue;
      init.anchor = candidate.id;
      break;
    }

    const WorldObject* anchor = init.anchor == kNoObject ? nullptr : world.find(init.anchor);
    const ClassIndex index = anchor == nullptr ? kNoClass : anchor->class_index;
    // The class's half first -- kind, flags, the two ceilings, and the six
    // economy numbers the class declares -- and the map's `<settlement>`
    // numbers over it wherever the element has one. That is the settlement
    // constructor's own rule (0x005c3e60): an argument of -1 means the class's
    // number, anything else is taken as written. Every shipped element writes
    // all six, so on a retail map the class only decides `max_units`.
    //
    // The two ceilings were carried from the class for a while and the six
    // numbers from nowhere: every settlement on every retail map started with
    // gold 0 and food 0 whatever the scenario declared -- Balcans says 2,500
    // and 200 for each stronghold -- so no computer player could ever buy a
    // barracks, and `max_units = 0` made `Holder::full()` true before the first
    // unit walked in.
    fill_settlement_class_defaults(*classes, index, init);
    if (declared != nullptr) {
      const auto take = [](std::int32_t authored, std::int32_t& into) {
        if (authored >= 0) into = authored;
      };
      take(declared->population, init.population);
      take(declared->max_population, init.max_population);
      take(declared->gold, init.gold);
      take(declared->food, init.food);
      take(declared->max_gold, init.max_gold);
      take(declared->max_food, init.max_food);
      take(declared->extra_sentries, init.extra_sentries);
    }

    // **Through `EconomySystem`, not through the store.** The store's `create`
    // appends a row and stops; the system's also resolves production rates,
    // sentry caps, loyalty, the stats vector -- and seeds every interval
    // timers to their periods.
    //
    // This called the store directly for a while, so every settlement on every
    // retail map started with every timer at zero, and `EconomySystem::
    // advance` never selects a timer already at zero as its next sub-step. The
    // result was that the same game time delivered as `[800]` and as
    // `[400, 400]` produced different worlds: **partition invariance broke on
    // every shipped map**, which is a determinism failure and not a cosmetic
    // one. The conformance harness found it; nothing else could have, because
    // `test_economy.cpp` builds its settlements through `EconomySystem::create`
    // and so never walks the path every real map takes.
    const SettlementId made = economy.create(world, init);

    // **Every building the map put inside the `<settlement>`, not only the
    // first.** The original's settlement keeps a deque of its member buildings
    // (`[set+0x6c..0x78]`), which is what `BestBarrack`, `RepairAll`, the
    // repair cursor and the sentry count walk -- and this engine's `buildings`
    // held the anchor alone on every loaded map, so `BestBarrack(5)` answered
    // an invalid handle for a stronghold with a barracks standing in it, and
    // no computer player has ever trained a soldier from a map's own town.
    // Ascending id, which is document order, which is the deque's order.
    for (const WorldObject& candidate : world.objects()) {
      if (candidate.internal != InternalKind::none) continue;
      if (candidate.settlement != slot.id || candidate.id == init.anchor) continue;
      if (!candidate.state.flags.is_building) continue;
      std::int32_t max_health = 0;
      if (candidate.class_index != kNoClass) {
        (void)parse_int(classes->property(candidate.class_index, "maxhealth"), max_health);
      }
      (void)economy.add_building(world, made, candidate.id, max_health);
    }
  }

  // The regions and the AI's nodes over them, built once the settlements
  // exist and never again: both are a function of the terrain layer and the
  // settlement list *as it stands now*. A load rebuilds the regions and
  // restores the nodes, whose centres a razed town hall no longer shows.
  // `sim/gaika.hpp` records which half of this is an approximation and why.
  world.mutable_lsa().build(world.terrain(), movement.grid());
  world.mutable_gaika().build(world, world.lsa(), economy.settlements());
}

std::uint32_t GameSession::Impl::chunk_for(std::string_view path) {
  if (const auto it = chunks.find(path); it != chunks.end()) return it->second;
  if (broken.find(path) != broken.end()) return script::kNoChunk;
  if (scripts == nullptr) return script::kNoChunk;

  const std::span<const std::byte> source = scripts->source(path);
  if (source.empty()) {
    // Not an error. Four `<behavior script=...>` bindings in the shipped class
    // tree name files that are not in `data.pak`, and the original runs anyway.
    broken.emplace(std::string(path), "no source");
    return script::kNoChunk;
  }

  const Result<script::Script> parsed = script::parse(source, path);
  if (!parsed.ok()) {
    broken.emplace(std::string(path), "parse failed");
    return script::kNoChunk;
  }
  const Result<script::Chunk> compiled = script::compile(parsed.value(), registry);
  if (!compiled.ok()) {
    broken.emplace(std::string(path), "compile failed");
    return script::kNoChunk;
  }
  const std::uint32_t index = scheduler.add_chunk(compiled.value());
  chunks.emplace(std::string(path), index);
  return index;
}

/// Run `hook`'s script on `subject`, to completion, now.
///
/// The lookup is the original's: resolve the subject's class, ask its resolved
/// method table for the hook's `sig`, and do nothing at all when it is not
/// there -- `0x0059b060` compares the map find against `end()` and returns.
/// The miss is a real answer and not worth a diagnostic: after resolution 203
/// of the installation's 845 classes answer `ondie` and 271 answer each of the
/// other two, so a class with nothing bound is what most of the graph is.
///
/// **Synchronous, not spawned.** All three launchers reach `0x006a0360`, which
/// calls 0x0069dca0: a context is built, the interpreter runs it with a budget
/// of `0x0fffffff`, and the context is deleted before the launcher returns. So
/// the hook has finished before the routine that fired it takes its next step
/// -- which is what lets `CMERCENARY_ONDIE.VS` read `.hero` on a unit that the
/// death virtual detaches from its hero immediately afterwards. See
/// `script::Scheduler::call`, which also says what a hook that tries to sleep
/// gets.
///
/// **No owner, and the subject as the first argument.** `0x006a0360` takes the
/// handle as one of the two words it hands the interpreter and binds no
/// receiver, so a hook is never one of its object's own coroutines and nothing
/// that reaps an object's scripts can reach it.
bool GameSession::Impl::run_class_hook(ClassHook hook, ObjectId subject, ObjectId argument) {
  if (classes == nullptr) return false;
  const WorldObject* slot = world.find(subject);
  if (slot == nullptr || slot->class_index == kNoClass) return false;

  const ClassHookShape shape = class_hook_shape(hook);
  for (const ClassMethod& method : classes->resolved_methods(slot->class_index)) {
    if (method.sig != shape.sig) continue;
    const std::uint32_t chunk = chunk_for(method.vs);
    if (chunk == script::kNoChunk) return false;
    // Both parameters are plain object handles. The second is the victim for
    // `onkill` and the settlement for `onenter`; `ondie` declares one
    // parameter and never sees it.
    const script::Value args[] = {
        script::Value::object(script::ObjectRef{kTypeObj, subject}),
        script::Value::object(argument == kNoObject ? script::ObjectRef{}
                                                    : script::ObjectRef{kTypeObj, argument}),
    };
    const std::span<const script::Value> passed(args, shape.arity);
    const script::CallReport ran = scheduler.call(chunk, passed);
    if (ran.id == script::kNoScript) return false;
    // Counted as the spawned hook used to be, started and then finished, so
    // the session's totals mean what they meant; and a trap is tallied where
    // every other script's is, because a hook that traps is as much a gap.
    ++started;
    if (ran.status == script::ExecStatus::finished) ++finished;
    if (ran.status == script::ExecStatus::failed) {
      script::RunReport report;
      report.traps.push_back(
          script::FailedScript{ran.id, scheduler.chunk(chunk).source_name, ran.trap});
      collect_traps(report);
    }
    return true;
  }
  return false;
}

void GameSession::Impl::collect_traps(const script::RunReport& report) {
  for (const script::FailedScript& failure : report.traps) {
    // The message a human acts on is the trap's detail plus where it happened;
    // the script id varies run to run and would make two reports differ for no
    // reason, so only the first is kept.
    std::string message(failure.trap.detail);
    if (message.empty()) message = "trap";
    if (!failure.source_name.empty()) {
      message += "  (";
      message += failure.source_name;
      message += ")";
    }
    TrapTally& tally = traps[message];
    ++tally.scripts;
    if (tally.first == script::kNoScript) tally.first = failure.id;
  }
}

GameSession::GameSession() : impl_(std::make_unique<Impl>()) {}
GameSession::~GameSession() = default;

World& GameSession::world() noexcept { return impl_->world; }
script::Scheduler& GameSession::scheduler() noexcept { return impl_->scheduler; }

void GameSession::set_debug_sink(DebugSink* sink) noexcept { impl_->context.debug = sink; }

HostContext& GameSession::host_context() noexcept { return impl_->context; }

SelectionTable& GameSession::selections() noexcept { return impl_->selections; }

void GameSession::set_local_player(PlayerId id) noexcept { impl_->context.local_player = id; }

Result<std::unique_ptr<GameSession>> GameSession::create(const script::HostRegistry& registry,
                                                         const SessionInputs& inputs,
                                                         std::uint32_t seed) {
  if (inputs.classes == nullptr) return FormatError::malformed;

  auto session = std::unique_ptr<GameSession>(new GameSession());
  Impl& impl = *session->impl_;
  impl.registry = &registry;
  impl.scripts = inputs.scripts;
  impl.game_properties.assign(inputs.game.begin(), inputs.game.end());
  impl.map_properties.assign(inputs.map_properties.begin(), inputs.map_properties.end());
  impl.terrain.assign(inputs.terrain.begin(), inputs.terrain.end());
  impl.height.assign(inputs.height.begin(), inputs.height.end());
  impl.settlement_templates.assign(inputs.settlement_templates.begin(),
                                   inputs.settlement_templates.end());
  impl.decor.assign(inputs.decor.begin(), inputs.decor.end());
  if (!impl.decor.empty()) {
    if (const Result<Grid> parsed = Grid::parse_mutable(impl.decor); parsed.ok()) impl.decor_grid = parsed.value();
  }
  impl.settlement_template_terrain.assign(inputs.settlement_template_terrain.begin(),
                                          inputs.settlement_template_terrain.end());
  impl.terrain_table.assign(inputs.terrain_table.begin(), inputs.terrain_table.end());
  impl.decor_table.assign(inputs.decor_table.begin(), inputs.decor_table.end());
  impl.masks = inputs.masks;
  if (!inputs.game.empty()) {
    if (const auto game = GameProperties::parse(inputs.game); game.ok()) {
      impl.season = season_from_name(game->season);
    }
  }
  impl.classes = inputs.classes;
  impl.entities = inputs.entities;

  // -- optional inputs, each of which degrades rather than failing ---------

  if (!inputs.ai_profile.empty()) {
    const Result<AiProfile> profile = AiProfile::parse(inputs.ai_profile);
    if (!profile.ok()) return profile.error();
    impl.profile = profile.value();
  }

  if (!inputs.translations.empty()) {
    const Result<game::TranslationTable> table =
        game::TranslationTable::parse(inputs.translations);
    if (!table.ok()) return table.error();
    impl.translations = table.value();
  }
  for (const std::span<const std::byte> document : inputs.localisations) {
    // A container's `Local/<language>/` holds translation tables and, beside
    // them, sounds and things that are not; only a table merges.
    const Result<game::TranslationTable> table = game::TranslationTable::parse(document);
    if (table.ok()) (void)impl.translations.merge(table.value());
  }

  if (!inputs.formations.empty()) {
    // A malformed table is refused rather than ignored: an empty one makes
    // every formation lookup answer null, which is the state that kept
    // `place_army` from ever placing anybody.
    Result<FormationTable> table = FormationTable::parse(inputs.formations);
    if (!table.ok()) return table.error();
    impl.movement.set_formations(std::move(table.value()));
  }

  if (!inputs.constants.empty()) {
    const Result<IniDocument> ini = IniDocument::parse(inputs.constants);
    if (!ini.ok()) return ini.error();
    impl.env.load_constants(ini.value());
    // The four `[GamePlay]` keys that tier a building's damage. `gbr.exe`
    // reads them by name (0x004db730) and refuses to start without them; this
    // keeps its compiled defaults for a session with no `CONST.INI` and takes
    // the file's numbers when there is one, which is what every other value
    // read out of that file does. A key the file does not have leaves the
    // default in place rather than zeroing the threshold.
    BuildingStateRules tiers = impl.world.building_state_rules();
    const auto read = [&](const char* key, std::int32_t& into) {
      std::int32_t value = 0;
      if (impl.env.constant(key, value)) into = value;
    };
    read("BuildingStateThreshold0", tiers.threshold0);
    read("BuildingStateThreshold1", tiers.threshold1);
    read("BuildingStateThreshold2", tiers.threshold2);
    read("BuildingStateHysteresis", tiers.hysteresis);
    impl.world.set_building_state_rules(tiers);
  }

  // The conquest's territory graph. Without this nothing ever calls
  // `CampaignSystem::configure`, and the campaign system is a complete,
  // registered, unreachable subsystem -- `ConquestBonus` answering "" and
  // `SetTerritoryState` finding no territory to set, on the one container in
  // the installation that has seven of them.
  if (!inputs.conquest.empty()) {
    const Result<ConquestMap> conquest = ConquestMap::parse(inputs.conquest);
    if (!conquest.ok()) return conquest.error();
    impl.campaign.configure(conquest.value());
  }
  // The note catalogue, map's document first so that it wins a duplicate id.
  // A refusal here is a refusal of the session: an unreadable `Notes.xml`
  // means `GiveNote` would silently give nothing for the whole mission, which
  // is a mission with no objectives on the list and no diagnostic anywhere.
  for (const std::span<const std::byte> document : {inputs.map_notes, inputs.notes}) {
    const Result<std::size_t> added = impl.campaign.notes().add(document);
    if (!added.ok()) return added.error();
  }
  // And the conversation catalogue, on the same terms and for the same reason:
  // a `RunConv` that finds nothing plays no line and says nothing, and there
  // are 74 of those on the critical path of a campaign.
  for (const std::span<const std::byte> document : inputs.conversations) {
    const Result<std::size_t> added = impl.campaign.conversations().add(document);
    if (!added.ok()) return added.error();
  }
  if (!inputs.player_setup.empty()) {
    const Status loaded = load_player_table(inputs.player_setup, impl.world.players());
    if (!loaded) return loaded.error();
  }

  // -- the world ----------------------------------------------------------

  impl.world.seed(seed);
  impl.world.set_class_graph(inputs.classes);
  // The same resolver `populate_from_map` is handed below, kept for the spawns
  // that arrive later: `Place` mints an object from a class name in the middle
  // of a match and has only the world to resolve its art through.
  impl.world.set_entity_resolver(inputs.entities);

  std::optional<MapObjectList> map_objects;
  std::optional<World::PopulateReport> map_loaded;
  if (!inputs.map_objects.empty()) {
    Result<MapObjectList> objects = MapObjectList::parse(inputs.map_objects);
    if (!objects.ok()) return objects.error();
    map_objects = std::move(objects.value());
    map_loaded = impl.world.populate_from_map(*map_objects, *inputs.classes, inputs.entities);
    impl.populated = *map_loaded;
    // The trigger regions, after the loader has bound the alias names they are
    // reached by: `load_areas` resolves each area through `NamedObjectTable`
    // rather than reproducing the loader's `num` -> id numbering. See
    // `sim/area.hpp`. Without this every `AreaCenter("Ruins")` in the
    // installation answers (-1, -1) -- a complete subsystem nothing reaches,
    // which is the failure mode this project has hit twice already.
    (void)load_areas(*map_objects, impl.world, impl.areas.areas());
  }

  // Systems in `kSystemOrder`, resolved by name so that the order is read from
  // the one place that states it rather than from the sequence of calls here.
  for (const std::string_view name : kSystemOrder) {
    System* system = nullptr;
    if (name == "movement") system = &impl.movement;
    else if (name == "combat") system = &impl.combat;
    else if (name == "economy") system = &impl.economy;
    else if (name == "feeder") system = &impl.feeder;
    else if (name == "hero") system = &impl.hero;
    else if (name == "env") system = &impl.env;
    else if (name == "command") system = &impl.command;
    else if (name == "match") system = &impl.match;
    else if (name == "ai") system = &impl.ai;
    else if (name == "campaign") system = &impl.campaign;
    else if (name == "areas") system = &impl.areas;
    else if (name == "fog") system = &impl.fog;
    // A name in `kSystemOrder` with no system behind it is a wiring mistake,
    // not a system with nothing to do.
    if (system == nullptr) return FormatError::malformed;
    impl.world.add_system(system);
  }

  // Two systems hold a pointer to something outside themselves, and neither
  // asks for it. Registering a system is not the same as feeding it, and this
  // is the third time in this file that distinction has cost something: the
  // economy's settlement table and the command table were both complete,
  // registered, and dead.
  //
  //   * `CommandSystem` launches a command's `<cmd script>` through the
  //     scheduler. Without it a queued command runs its costs and its verify
  //     and then does **nothing**, silently, for every order on every map.
  //   * `CombatSystem` reads unit properties -- attack, armour, range, the
  //     damage type -- from the class graph. Without it every combatant falls
  //     back to whatever a missing class means.
  //
  // Both are one line, and both were missing for as long as `GameSession` has
  // existed. Neither has a test that could have noticed, because a system's
  // own tests construct it and hand it what it needs.
  impl.command.set_scheduler(&impl.scheduler);
  // **The fifth of the same shape.** A command resolves to a `.vs` path through
  // the class graph and then has to have it compiled; without this the
  // scheduler's own lookup misses every class `<method>` -- only the `[Scripts]`
  // manifest and the per-object `idle` methods are ever preloaded -- and
  // `service` retires the command silently. Every `move`, `enter` and `engage`
  // on every map was a no-op.
  impl.command.set_library(&impl);
  impl.combat.set_class_graph(inputs.classes);
  // **The sixth.** `MoveState::speed` is documented as "copied in by the
  // loader" and no loader ever copied it, so every unit in every session had a
  // speed of zero: it would lay a path and walk none of it.
  impl.movement.set_class_graph(inputs.classes);

  // The fourth. `CombatSystem` was complete, tested and registered, and `add`
  // had no caller outside `engine/tests` -- so `units_` was empty in every real
  // session and `advance` returned on its first line. Two hostile armies could
  // stand next to each other for a thousand turns and nobody died. Binding it
  // to the world makes `start` and every `advance` reconcile the population
  // against `World::objects()`, which is ascending id order, which is map order.
  impl.combat.set_world_bound(true);

  // And the fifth. `MovementSystem::set_grid` had no caller either, so A* ran
  // over an empty `ObstructionGrid` and units walked through walls and water.
  // The map's `Terrain.pass.grid` is the authored bitmap; `from_grid` refuses
  // anything that is not 16 units and one bit per cell rather than
  // reinterpreting a coarser layer. A refusal degrades to the empty grid, which
  // is where this started, rather than failing the session.
  // The terrain-type layer, which `IsPointInWater` reads and nothing else does.
  // A layer that does not parse degrades to none, which answers "not water"
  // everywhere -- the same thing an absent one does, and what every session did
  // before the field existed.
  // Writable over the session's own copy: the templates lay their ground
  // into it at `start_match`, and the world's view is the same bytes.
  if (!impl.terrain.empty()) {
    if (const Result<Grid> grid = Grid::parse_mutable(impl.terrain); grid.ok()) {
      impl.world.set_terrain(grid.value());
    }
  }

  // The height layer, which `GetTerrainHeight` reads and nothing else does. A
  // layer that does not parse degrades to none, which answers sea level
  // everywhere -- the same thing an absent one does.
  if (!impl.height.empty()) {
    if (const Result<Grid> grid = Grid::parse_mutable(impl.height); grid.ok()) {
      impl.world.set_height(grid.value());
    }
  }

  if (!inputs.passability.empty()) {
    if (const Result<Grid> grid = Grid::parse(inputs.passability); grid.ok()) {
      if (Result<ObstructionGrid> obstruction = ObstructionGrid::from_grid(grid.value());
          obstruction.ok()) {
        impl.movement.set_grid(std::move(obstruction.value()));
        // And the layer itself, writable, for the rebuild at `start_match`.
        if (Result<OwnedGrid> owned = OwnedGrid::copy(grid.value()); owned.ok()) {
          impl.pass_layer = std::move(owned.value());
        }
      }
    }
  }

  // The economy keeps its own settlement table and `populate_from_map` does not
  // fill it: the loader spawns the settlement/holder/warehouse triple as world
  // objects and stops. Without this, every `Settlement` handle a script gets
  // back resolves to nothing -- which is how `IsIndependent` failed 25 times on
  // Numantia the first time a map ran.
  impl.seed_economy(map_objects.has_value() ? &map_objects.value() : nullptr,
                    map_loaded.has_value() ? &map_loaded.value() : nullptr);

  // The command table, before anything can resolve an order against it.
  for (const std::span<const std::byte>& document : inputs.commands) {
    if (document.empty()) continue;
    (void)impl.command.mutable_table().merge(document);
  }

  // The item catalogue, before any item can be minted or drawn. A document
  // that does not parse degrades to no catalogue, which is what every session
  // ran with before the field existed.
  {
    bool loaded = !inputs.items.empty() && impl.items.load(inputs.items).ok();
    if (!inputs.custom_items.empty() && impl.items.load(inputs.custom_items).ok()) loaded = true;
    if (loaded) impl.hero.items().set_catalog(&impl.items);
  }

  impl.env.units() = shipped_unit_catalog();
  for (std::size_t index = 0; index < kPlayerCount; ++index) {
    const PlayerId id = static_cast<PlayerId>(index);
    const PlayerSetup& setup = impl.world.players().setup(id);
    // **Seeded under the script's number, not the `PlayerId`.** `EnvSystem`'s
    // AI-variable store keys on whatever the script passes, and a script passes
    // `AIPlayer` from `AIGetPlayer`, which is 1-based like every other player
    // number a script sees. Seeding this with the 0-based index put every
    // player's tuning one slot low, so `AIVar(1, AIV_NoRecruit)` read player
    // two's -- silently, because the store answers a missing key with zero and
    // an AI tuned entirely to zeros still runs. `RECRUITER.VS` and
    // `SQUADMONITOR.VS` read a dozen of these each.
    impl.env.seed_ai_vars(player_to_script(id), impl.profile,
                          ai_difficulty_overlay(setup.difficulty));
  }

  // -- the script side ----------------------------------------------------

  // Without this, 42 of the 245 script globals refuse: the `SS_`, `GS_`, `ES_`
  // and `TS_` families are declared in `AI.INI`, not in the executable, so
  // `Host::global` has to be able to reach the parsed profile. It hangs on the
  // host rather than on `HostContext` because `Host::global` is handed a name
  // and no call context, and because a profile is load-time data rather than
  // world state.
  impl.host.set_ai_profile(&impl.profile);
  // The AI reaches its scripts through this table, keyed by the profile name a
  // `playerdata/@AI` names. The empty name is the root `data/ai/`.
  impl.ai.add_profile("", &impl.profile);

  impl.context.world = &impl.world;
  impl.context.object_type = kTypeObj;
  impl.context.translations = &impl.translations;
  impl.context.selections = &impl.selections;
  // The compile-on-demand seam. Only `RunAIHelper` uses it, and only because
  // its file name comes from a running script rather than from a manifest.
  impl.context.library = &impl;

  impl.scheduler.set_registry(&registry);
  impl.scheduler.set_host(&impl.host);
  impl.scheduler.set_user(&impl.context);
  // Without this the collection pool grows for the life of the match; see
  // `sim/objlist.hpp`, which spells out both halves of the lifetime rule.
  install_objlist_lifetime(impl.scheduler);
  // And the other half of the scheduler's lifetime seam: the per-step hook that
  // performs an `Erase` a script asked for on its own object. See
  // `install_deferred_erase`; without it a self-erase is armed and never fired,
  // and six shipped behaviours simply never take their object off the field.
  install_deferred_erase(impl.scheduler);
  // And the seam that lets a *system* start a script. Wired here rather than in
  // `SessionInputs` because the implementation is this session: `Impl` is the
  // only thing that holds the class graph, the chunk cache and the scheduler at
  // once. A world without it fires no class hook and takes the `ondie` latch
  // all the same, which is what every synthetic test gets. See `sim/hooks.hpp`.
  impl.world.set_hook_runner(&impl);

  impl.world.start();

  // The authored per-object attributes the systems' records can only take
  // once the systems have started: a unit's `Level`, a hero's `hs*` skills
  // and `slot<n>` items. Everything the world itself holds -- `stamina`,
  // `amount`, `display_name` -- went in at `spawn_map_object`.
  if (map_objects.has_value() && map_loaded.has_value()) {
    impl.apply_authored_attributes(*map_objects, map_loaded->object_ids);
  }
  return session;
}

/// What the unit loader (0x005dd3a0) and the hero loader (0x00530810) do
/// with the attributes their classes own, read off the executable:
///
///   * `Level="L"` -- 0x005dd510 parses it and calls the unit's
///     `SetExperience` (`vtbl+0x118`, 0x005dbf70: `[unit+0x180] = arg`) with
///     `exp_table[L + 1]` (0x009bf87c, the running-total threshold per
///     level, indexed from 1), and `level_for_experience` (0x005d28c0) of
///     that is `L + 1`. So the map's `Level` is **0-based** and the unit's
///     level is one more: the 16,171 shipped units read 0..59, and a
///     `Level="9"` legionary is level 10. Here that is `set_level(L + 1)`,
///     the earned level, with the progress counter at zero -- the
///     threshold exactly.
///   * `hs<Skill>="n"` -- `load_skill`; the balance derives from the level.
///   * `slot<n>="<item id>"` -- 0x005af5d0 takes every attribute whose name
///     starts `slot`, in document order, mints the item by id (0x00535640
///     tries the catalogue by one key and then the other) and hands it to
///     `AddItem` (`vtbl+0xe8`). `inventorysize` beside it is parsed and
///     dropped. Here: `ItemSystem::add`, which refuses a full holder exactly
///     as the original's `AddItem` does.
void GameSession::Impl::apply_authored_attributes(const MapObjectList& map,
                                                  std::span<const ObjectId> object_ids) {
  for (std::size_t i = 0; i < map.objects().size() && i < object_ids.size(); ++i) {
    const ObjectId id = object_ids[i];
    if (id == kNoObject) continue;
    const MapObject& placed = map.objects()[i];
    const WorldObject* slot = world.find(id);
    if (slot == nullptr) continue;

    std::int32_t level = 0;
    if (slot->state.flags.is_unit && parse_authored_int(placed.attribute("Level"), level) &&
        level >= 0) {
      (void)hero.set_level(id, level + 1);
    }
    for (const auto& [name, value] : placed.attributes) {
      if (name.size() > 2 && name[0] == 'h' && name[1] == 's' && slot->state.flags.is_hero) {
        const std::int32_t which = hero_skill_id(name);
        std::int32_t points = 0;
        if (which >= 0 && parse_authored_int(value, points)) {
          (void)hero.load_skill(id, static_cast<HeroSkill>(which), points);
        }
      } else if (name.size() > 4 && name.compare(0, 4, "slot") == 0 && !value.empty()) {
        (void)hero.items().add(world, id, value);
      }
    }
  }
}

/// The passability layer rebuilt over `rect` from what the world holds now
/// -- the original's 0x00547700, run over the whole map at match start
/// (0x00552e95) once the templates have landed -- and the obstruction grid
/// made from the result. Every input must be there or nothing happens and
/// the grid stays the map's; `report` says which and how much changed.
bool GameSession::Impl::rebuild_passability(const edit::WorldRect& rect, PassabilityReport& report) {
  Impl& impl = *this;
  if (impl.pass_layer.grid().cell_size() == 0 || impl.world.terrain().cell_size() == 0) return false;
  if (impl.terrain_table.empty() || impl.masks == nullptr || impl.entities == nullptr ||
      impl.classes == nullptr) {
    return false;
  }
  const Result<TerrainTable> terrains = TerrainTable::parse(impl.terrain_table);
  if (!terrains.ok()) return false;

  // The decoration kinds' masks, once.
  const Grid& decor = impl.decor_grid;
  if (impl.decor_masks.empty() && !impl.decor_table.empty()) {
    impl.decor_masks.assign(256, nullptr);
    if (const Result<DecorTable> kinds = DecorTable::parse(impl.decor_table); kinds.ok()) {
      for (const DecorKind& kind : kinds->kinds()) {
        if (kind.type <= 0 || kind.type >= 256) continue;
        impl.decor_masks[static_cast<std::size_t>(kind.type)] =
            impl.masks->mask_of(impl.entities->resolve(kind.entity));
      }
    }
  }

  // Every standing object with a mask, at its position. The mask is the
  // class's entity's in the map's season, resolved once per class.
  std::vector<edit::Footprint> footprints;
  for (const WorldObject& object : impl.world.objects()) {
    if (object.object == nullptr || object.class_index == kNoClass) continue;
    if (object.state.flags.unspawned || object.state.is_held()) continue;
    auto found = impl.class_masks.find(object.class_index);
    if (found == impl.class_masks.end()) {
      const std::string_view path = impl.classes->entity_path(object.class_index, impl.season);
      const Entity* entity = path.empty() ? nullptr : impl.entities->resolve(path);
      found = impl.class_masks.emplace(object.class_index, impl.masks->mask_of(entity)).first;
    }
    if (found->second != nullptr) footprints.push_back({found->second, object.state.position});
  }

  edit::rebuild_passability(impl.pass_layer.grid(), rect, impl.world.terrain(), terrains.value(),
                            impl.world.height(), footprints, decor, impl.decor_masks);
  Result<ObstructionGrid> obstruction = ObstructionGrid::from_grid(impl.pass_layer.grid());
  if (!obstruction.ok()) return false;

  // How far the result is from the grid in use: on an authored map, nowhere.
  const ObstructionGrid& before = impl.movement.grid();
  std::size_t changed = 0;
  for (std::int32_t y = 0; y < obstruction->height(); ++y) {
    for (std::int32_t x = 0; x < obstruction->width(); ++x) {
      changed += before.blocked_cell(x, y) != obstruction->blocked_cell(x, y);
    }
  }
  impl.movement.set_grid(std::move(obstruction.value()));
  report.rebuilt = true;
  report.cells_changed += changed;
  return true;
}

void GameSession::Impl::lay_template_ground(std::span<const TemplateStamp> stamps) {
  Impl& impl = *this;
  if (stamps.empty() || impl.settlement_template_terrain.empty() || impl.settlement_templates.empty()) {
    return;
  }
  const Result<Grid> source = Grid::parse(impl.settlement_template_terrain);
  const Result<SettlementTemplateLibrary> library =
      SettlementTemplateLibrary::parse(impl.settlement_templates);
  if (!source.ok() || !library.ok()) return;
  // The world's grids view the session's own bytes; the writable views are
  // made over the same.
  Result<Grid> terrain = Grid::parse_mutable(impl.terrain);
  Result<Grid> height = impl.height.empty() ? Result<Grid>(FormatError::not_found) : Grid::parse_mutable(impl.height);
  if (!terrain.ok()) return;
  std::vector<std::uint8_t> marks;
  GroundLayers layers;
  layers.terrain = &terrain.value();
  layers.height = height.ok() ? &height.value() : nullptr;
  layers.decor = impl.decor_grid.cell_size() != 0 ? &impl.decor_grid : nullptr;
  layers.marks = &marks;
  for (const TemplateStamp& stamp : stamps) {
    if (stamp.index >= library->templates().size()) continue;
    const TemplateGround laid =
        stamp_template_ground(layers, source.value(), library->templates()[stamp.index], stamp.origin);
    if (!laid.any()) continue;
    // The box in map units, grown by the shore's margin and as far as the
    // limiter's slope can reach past it (288 plus 255 / 20 cells of 32).
    const std::int32_t dx = stamp.origin.x - library->templates()[stamp.index].centre.x;
    const std::int32_t dy = stamp.origin.y - library->templates()[stamp.index].centre.y;
    constexpr std::int32_t kReach = 768;
    impl.ground.changed.push_back(edit::WorldRect{laid.box.x0 + dx - kReach, laid.box.y0 + dy - kReach,
                                                  laid.box.x1 + dx + kReach, laid.box.y1 + dy + kReach}
                                      .clamped(edit::WorldRect::of_map(impl.world.terrain())));
    impl.ground.cells_copied += laid.cells_copied;
    impl.ground.cells_levelled += laid.cells_levelled;
    impl.ground.decorations_bulldozed += laid.decorations_bulldozed;
    impl.ground.shore_cells += laid.shore_cells;
    impl.ground.cells_sloped += laid.cells_sloped;
  }
}

std::size_t GameSession::start_match(const MatchOptions& options) {
  Impl& impl = *impl_;
  MatchRules rules;
  if (!impl.game_properties.empty()) {
    if (auto parsed = MatchRules::parse_game_properties(impl.game_properties); parsed.ok()) {
      rules = parsed.value();
    }
  }
  // And the two fields `parse_game_properties` deliberately does not touch,
  // because they are in a different document. `MatchRules` has carried the
  // comment "the caller fills them in" since the match domain was written and
  // no caller ever did, so `map_size` was **zero in every session** -- which
  // makes the map rectangle `0 .. -1`, empty, and `Place` refuse every position
  // in the world. See `SessionInputs::map_properties`.
  if (!impl.map_properties.empty()) {
    // The `ok()` is belt and braces and is kept knowingly: `Result::value()` on
    // a failed parse hands back a default-constructed `MapGeometry`, so
    // dropping the check writes the same zeroes. A fault injected into it
    // survives the whole suite for that reason -- which is worth writing down
    // rather than leaving as an apparent gap in the tests. The guarantee
    // belongs to `Result`, not to this call site, and a call site that relies
    // on somebody else's guarantee without saying so is one refactor from
    // relying on nothing.
    if (auto geometry = MapGeometry::parse(impl.map_properties); geometry.ok()) {
      rules.map_name = geometry.value().name;
      // `size_x`, not `size_y`: the two are equal in every shipped map and
      // `MapSize` (0x0051b120) returns a single field.
      rules.map_size = geometry.value().size_x;
    }
  }
  (void)setup_match(impl.world, impl.match, rules, options);

  // The placeholders, now that every player has a race. The library is parsed
  // here and dropped: it is read once per match.
  impl.materialised = MaterialiseReport{};
  if (!impl.settlement_templates.empty()) {
    const Result<SettlementTemplateLibrary> library =
        SettlementTemplateLibrary::parse(impl.settlement_templates);
    if (library.ok()) {
      impl.materialised = materialise_mutable_settlements(impl.world, impl.economy, impl.match,
                                                          library.value(), impl.entities);
    }
  }
  // Each template's ground where it landed, in the order they landed
  // (0x0058e3c0's tail, run per template as it is placed): the terrain and
  // the height under the town are the template's, the decorations are
  // bulldozed, the shore is taken out. Laid after the members rather than
  // between them, which lays the same ground -- nothing a member does reads
  // the layers on the way.
  impl.ground = GroundReport{};
  impl.lay_template_ground(impl.materialised.stamps);
  // The passability layer, whole, over the world as it stands now
  // (0x00552e95): the templates' footprints in, the placeholders' out. On an
  // authored map with no placeholder this reproduces the shipped layer cell
  // for cell -- `immap passability` proves it over the corpus -- so the
  // grid only ever changes where the original's would.
  impl.passability = PassabilityReport{};
  (void)impl.rebuild_passability(edit::WorldRect::of_map(impl.world.terrain()), impl.passability);
  if (impl.materialised.strongholds + impl.materialised.villages > 0 ||
      impl.passability.cells_changed > 0) {
    // The anchors moved and the settlements changed class, and the grid the
    // partition is cut over may have too: both tables are functions of the
    // settlement list and the grid. See `seed_economy`.
    impl.world.mutable_lsa().build(impl.world.terrain(), impl.movement.grid());
    impl.world.mutable_gaika().build(impl.world, impl.world.lsa(), impl.economy.settlements());
  }
  // The setup's rules over the materialised settlements, in the original's
  // one loop (0x005267b3..0x005267dd): a starting gold other than "Default"
  // is `SetGold` on every settlement with a warehouse, the players' and the
  // independents' alike, clamped as `SetGold` clamps; the world population
  // scales every non-wildlife settlement's `max_population` by its percent,
  // integer division, and leaves `population` alone. Then exploration off
  // reveals the map (0x00526813).
  const MatchRules& chosen = impl.match.rules();
  for (Settlement& town : impl.economy.settlements().all()) {
    if (town.owner == kNeutralWildlife) continue;
    if (chosen.starting_gold >= 0 && town.warehouse.object != kNoObject) {
      (void)impl.economy.set_resource(town.id, Resource::gold, chosen.starting_gold);
    }
    if (chosen.world_population != 100) {
      town.max_population = town.max_population * chosen.world_population / 100;
    }
  }
  // The exploration map is sized from the match's `map_size`, and the fog
  // system started with the world -- before any match was configured, so
  // its map was **empty in every session**, "explored nowhere", and every
  // `IsExplored` answered no. Started again here, over the rules it needs.
  if (FogSystem* fog = fog_system_of(impl.world); fog != nullptr) {
    fog->start(impl.world);
    if (!chosen.exploration) fog->map().explore_all();
  }

  // The condition's script has to be in the library before anything can spawn
  // it: `spawn_by_name` looks up, it does not compile.
  const std::string path = victory_script_path(chosen.condition);
  if (path.empty() || impl.chunk_for(path) == script::kNoChunk) return 0;
  return start_victory_scripts(impl.world, impl.scheduler);
}

MatchStatus GameSession::match_status() const { return sim::match_status(impl_->world); }
MatchStatus GameSession::match_status(PlayerId viewer) const {
  return sim::match_status(impl_->world, viewer);
}

void GameSession::declare_match(PlayerId player, bool lost) {
  if (MatchSystem* match = match_system_of(impl_->world)) match->end_game(player, lost);
}

const MaterialiseReport& GameSession::materialised() const noexcept { return impl_->materialised; }
const PassabilityReport& GameSession::passability() const noexcept { return impl_->passability; }
const GroundReport& GameSession::ground() const noexcept { return impl_->ground; }
const Grid& GameSession::decor() const noexcept { return impl_->decor_grid; }
const World::PopulateReport& GameSession::populated() const noexcept { return impl_->populated; }

std::size_t GameSession::start_ai() {
  Impl& impl = *impl_;
  // `AIRun` looks a script up, it does not compile one, so every file the AI
  // can reach must already be a chunk. The profile's `[Scripts]` table is that
  // manifest -- 65 files, `Main.vs` among them.
  for (const AiScriptDeclaration& declaration : impl.profile.scripts()) {
    (void)impl.chunk_for("DATA/AI/" + declaration.file);
  }
  return ai_start_players(impl.world, impl.scheduler);
}

std::size_t GameSession::start_bonuses(std::span<const std::string> scripts) {
  Impl& impl = *impl_;
  const EconomySystem* economy = economy_of(impl.world);
  if (economy == nullptr) return 0;
  std::size_t started = 0;
  for (PlayerId id = 0; id < kPlayerCount; ++id) {
    const std::int32_t bonus = impl.world.players().setup(id).bonus;
    if (bonus < 0) continue;
    // The file whose leading number is the bonus.
    std::string_view file;
    for (const std::string& name : scripts) {
      std::size_t end = 0;
      while (end < name.size() && name[end] >= '0' && name[end] <= '9') ++end;
      if (end == 0) continue;
      std::int32_t number = 0;
      for (std::size_t i = 0; i < end; ++i) number = number * 10 + (name[i] - '0');
      if (number == bonus) {
        file = name;
        break;
      }
    }
    if (file.empty()) continue;
    // The player's first stronghold.
    const Settlement* home = nullptr;
    for (const Settlement& settlement : economy->settlements().all()) {
      if (settlement.owner == id && settlement.kind == SettlementKind::stronghold) {
        home = &settlement;
        break;
      }
    }
    if (home == nullptr) continue;
    const std::uint32_t chunk = impl.chunk_for("DATA/BONUSSCRIPTS/" + std::string(file));
    if (chunk == script::kNoChunk) continue;
    const script::Value args[] = {script::Value::object(kTypeSettlement, home->object),
                                  script::Value::integer(player_to_script(id))};
    if (impl.scheduler.spawn(chunk, args, script::ObjectRef{}) != script::kNoScript) ++started;
  }
  return started;
}

std::size_t GameSession::start_object_scripts() {
  Impl& impl = *impl_;
  impl.object_scripts_wanted = true;
  if (impl.classes == nullptr || impl.scripts == nullptr) return 0;

  // Everything below the watermark has already been offered its scripts, so a
  // re-run only picks up what has appeared since.
  const auto offered = [&impl](const WorldObject& slot) {
    if (slot.id < impl.script_watermark) return false;
    if (slot.internal != InternalKind::none) return false;
    if (slot.class_index == kNoClass) return false;
    // A spawn template runs nothing. It is not in play, and an idle script is
    // the loudest thing an object does: `UNIT_IDLE.VS` polls `.AI` and picks
    // targets every pass, so leaving templates running would put a map's
    // whole reinforcement schedule on the scheduler from turn one. The copies
    // `SpawnGroup` mints get their scripts when they are spawned.
    return !slot.state.flags.unspawned;
  };

  std::size_t started = 0;
  // **The class's behaviours, every object's, before any object's idle.**
  //
  // `gbr.exe` starts behaviours from the object's start virtual (vtbl+0x2c,
  // 0x005aec40), which every factory path calls the moment an object is
  // constructed -- the map loader's, `Place`'s, a clone's -- and which the
  // unit, building and other overrides (0x0053c730, 0x004d64d0, 0x004db2d0,
  // 0x005d3090) call before doing anything of their own. It sizes the
  // object's script slots to one more than the class's behaviour count, asks
  // slot 0 -- the command slot -- to restart whatever its queue holds, which at
  // construction is nothing, and then spawns behaviour `i` into slot `i + 1`
  // through 0x006a07e0, with the object as the script's one argument. The
  // `idle` a class binds is not started there: it is the command queue's
  // default and starts when the object's queue is first found empty, which is
  // after construction. So a map's objects each have their behaviours before
  // any of them has an idle -- **INFERRED** from that structure, not traced to
  // the tick that first finds a queue empty.
  //
  // Skipped, as 0x005aec40 skips them, while a save is being loaded
  // (`[0x00a87390]`): the load restores the coroutines instead. The same
  // routine also skips in the editor (`[0x00a8734c]`), which never runs this.
  //
  // Spawn order is object order, which is ascending id order, which is the
  // order the map declared them in, and within an object slot order. Script
  // ids therefore reproduce across runs, and a script id is world state.
  for (const WorldObject& slot : impl.world.objects()) {
    if (!offered(slot)) continue;
    started += start_behaviors(impl.scheduler, impl, impl.world, slot.id);
  }
  // **Then each object's idle, unless it already has an order.** The idle is
  // its command slot's default -- `CommandSystem::end_resting_idle` says why
  // and where the original shows it -- so an object that was ordered before
  // this pass reached it, a trained soldier sent to its rally point in the
  // pass that made it, starts on the order, and its queue gives it the idle
  // when the order is done. Started here it ran beside the order.
  const CommandSystem* commands = command_system(impl.world);
  for (const WorldObject& slot : impl.world.objects()) {
    if (!offered(slot)) continue;
    if (commands != nullptr && commands->command_count(slot.id) > 0) continue;
    for (const ClassMethod& method : impl.classes->resolved_methods(slot.class_index)) {
      if (method.sig != kIdleMethod) continue;
      const std::uint32_t chunk = impl.chunk_for(method.vs);
      if (chunk == script::kNoChunk) break;
      const script::ObjectRef owner{kTypeObj, slot.id};
      // The receiver is the script's first argument: `UNIT_IDLE.VS` opens
      // `// void, Obj This`, and every idle script in the corpus does.
      const script::Value self = script::Value::object(owner);
      const script::Value args[] = {self};
      if (impl.scheduler.spawn(chunk, args, owner) != script::kNoScript) ++started;
      break;
    }
  }
  impl.script_watermark = impl.world.next_id();
  impl.started += started;
  return started;
}

ObjectId GameSession::script_watermark() const noexcept { return impl_->script_watermark; }

Status GameSession::restore_campaign(const CampaignCarry& carry) {
  CampaignSystem* campaign = campaign_system_of(impl_->world);
  if (campaign == nullptr) return Status(FormatError::malformed);
  return campaign->restore(carry);
}

CampaignCarry GameSession::campaign_carry(std::string_view container,
                                          const MissionResult& result) const {
  const CampaignSystem* campaign = campaign_system_of(impl_->world);
  if (campaign == nullptr) return CampaignCarry{std::string(container), {}, {}};
  return campaign->carry(container, result);
}

std::size_t GameSession::start_sequences(std::span<const SequenceRef> manifest,
                                         std::string_view base) {
  Impl& impl = *impl_;
  if (impl.scripts == nullptr) return 0;
  std::size_t started = 0;
  // Manifest order, because that is the order the container declares and a
  // script id is world state.
  CampaignSystem* campaign = campaign_system_of(impl.world);
  for (const SequenceRef& sequence : manifest) {
    const std::string path = sequence_entry_path(sequence.script, base);
    // Installed whether or not it compiles and whether or not it autoruns.
    // `RunSequence` has to be able to find it, and `Start`'s first branch --
    // no compiled script, status `"Finished"` -- is only reachable for a
    // sequence that is *in* the table.
    if (campaign != nullptr) {
      campaign->add_sequence(sequence.name, path);
      // Every declared sequence starts `"Waiting"`, which is what the
      // original's status getter writes back when it finds the key absent.
      //
      // **Do not skip this as an optimisation.** `IsWaiting` is a predicate
      // over the key, so an uninitialised sequence reads as *not* waiting, and
      // Britain's `WinCond` is `if (... || !IsWaiting("CalgacusArmy"))` --
      // which under the wrong default takes its victory branch on the first
      // pass, at turn zero. Behaving correctly by accident is worse than
      // behaving wrongly for a legible reason.
      set_sequence_waiting(impl.world, sequence.name);
    }
    // Compiled whether or not it autoruns: a sequence `RunSequence` will reach
    // for has to be in the library before it can be found, and a sequence that
    // does not compile is a gap worth reporting even if nothing starts it.
    const std::uint32_t chunk = impl.chunk_for(path);
    if (chunk == script::kNoChunk) continue;
    if (!sequence.autorun_allowed) continue;
    // No receiver and no arguments. Every shipped sequence opens `//void` with
    // an empty parameter list, and the retail engine runs them detached -- the
    // script thread's environment `Context` is null, which is what makes a
    // relative root-scope `Env*` key mean itself. See `sim/env.hpp`.
    const script::ScriptId id = impl.scheduler.spawn(chunk);
    if (id == script::kNoScript) continue;
    ++started;
    if (campaign != nullptr) {
      if (CampaignSystem::SequenceEntry* entry = campaign->find_sequence(sequence.name)) {
        entry->running = id;
      }
      set_sequence_running(impl.world, sequence.name);
    }
  }
  impl.started += started;
  return started;
}

void GameSession::advance(std::uint64_t turns, std::int32_t turn_length) {
  Impl& impl = *impl_;
  for (std::uint64_t i = 0; i < turns; ++i) {
    // Systems first, then the scripts that observe what they did. Both advance
    // by the same slice of game time, and the scheduler's clock is the one
    // `Sleep` counts against.
    impl.world.advance(turn_length);
    // Anything spawned since the last turn gets its class's `idle` method now,
    // before it is asked to do anything. `SpawnGroup` is what makes this more
    // than a load-time concern: a mission's reinforcements arrive mid-match and
    // a unit with no idle script is a unit that stands still forever.
    if (impl.object_scripts_wanted) start_object_scripts();
    // Before the scripts run, so nothing observes a selection holding an id
    // that this turn's combat despawned.
    impl.selections.prune(impl.world);
    // And no script resumes on one: a corpse that left the world this turn
    // takes its `idle` and command-method coroutines with it, as an `Erase`
    // already does. See `reap_departed`. What died in the systems is reaped
    // here, so the scheduler's step hook -- which reaps what a script kills --
    // starts the pass with nothing owed (an equivalence: without it the first
    // slice would only reap again and find nothing).
    if (CombatSystem* combat = combat_system_of(impl.world)) combat->forget_fresh_deaths();
    (void)reap_departed(impl.scheduler, impl.world);
    // The AI order queues' timer (0x0041d790), at the turn's time: a due queue
    // drains one order, and the `AIOSendSquad.vs` it spawns runs in the pass
    // below. Here, between the systems and the scripts, is the labelled part --
    // see `AiOrderQueue`.
    (void)run_ai_orders(impl.world, impl.scheduler, &impl, impl.world.time());
    const script::RunReport report = impl.scheduler.advance(turn_length);
    // Whichever sequences ended this pass are `"Finished"` now. The original
    // does it from a completion callback (0x005b9f50) that matches the ended
    // thread against each sequence's recorded id; this is the only place that
    // holds both the table and the scheduler.
    if (CampaignSystem* campaign = campaign_system_of(impl.world)) {
      campaign->reap_sequences(impl.world, impl.scheduler);
    }
    // And whichever AI helpers ended. The original erases the map entry from
    // the script-completion hook at `CVXAIHelper`'s vftable slot +0x1c
    // (0x004d2160); the scheduler's one teardown hook is already the `ObjList`
    // pool's, so this is polled beside the sequences for the same reason and
    // in the same place.
    impl.ai.reap_helpers(impl.scheduler);
    impl.finished += report.completed;
    impl.collect_traps(report);
    ++impl.turns;
  }
}

SessionReport GameSession::report() const {
  const Impl& impl = *impl_;
  SessionReport out;
  out.turns = impl.turns;
  out.scripts_started = impl.started;
  out.scripts_finished = impl.finished;
  out.scripts_running = impl.scheduler.live_count();
  out.hash = impl.world.hashes().hash_of_hashes;

  out.traps.reserve(impl.traps.size() + impl.broken.size());
  for (const auto& [message, tally] : impl.traps) {
    out.traps.push_back(SessionReport::Trap{message, tally.scripts, tally.first});
  }
  // A script that never compiled never traps, and is exactly as much of a gap.
  for (const auto& [path, why] : impl.broken) {
    out.traps.push_back(SessionReport::Trap{std::string(why) + ": " + path, 1, script::kNoScript});
  }

  for (const auto& [verb, count] : impl.command.launch_failures()) {
    out.launch_failures.push_back(SessionReport::LaunchFailure{verb, count});
  }

  // Most-hit first, then by message. Deterministic, so two runs diff cleanly.
  std::sort(out.traps.begin(), out.traps.end(),
            [](const SessionReport::Trap& a, const SessionReport::Trap& b) {
              if (a.scripts != b.scripts) return a.scripts > b.scripts;
              return a.message < b.message;
            });
  return out;
}

// --------------------------------------------------------------------------
// the saved game
// --------------------------------------------------------------------------
//
// See docs/formats/save.md. Two things this pair is responsible for that
// nothing below it can be:
//
//   1. **Routing the system sections.** `write_save` and `read_save` cannot
//      reach inside a `System` -- `System` has no `serialize` in its interface
//      and does not get one, because that would put a virtual on a seam ten
//      domains share. So the bytes come from here and go back here, and the
//      dispatch mirrors the one in `create` so that both read from
//      `kSystemOrder` rather than from a hand-kept sequence of calls.
//   2. **Priming the script library.** `Scheduler::deserialize` resolves a
//      saved coroutine's code **by source name** and refuses one it cannot
//      find. A session that has only been `create`d has an empty library, so
//      the world is restored first and the library is compiled from it, and
//      only then does the scheduler's section go in.

Result<std::vector<std::byte>> GameSession::save(std::string_view map) const {
  const Impl& impl = *impl_;

  // In `kSystemOrder`, which is registration order, which is the order
  // `SaveMeta::systems` records and `read_save` checks against.
  std::vector<SaveSection> sections;
  for (const std::string_view name : kSystemOrder) {
    SaveSection section;
    section.name.assign(name);
    if (name == "movement") impl.movement.serialize(section.payload);
    else if (name == "combat") impl.combat.serialize(section.payload);
    else if (name == "economy") impl.economy.serialize(section.payload);
    else if (name == "feeder") impl.feeder.serialize(section.payload);
    else if (name == "hero") impl.hero.serialize(section.payload);
    else if (name == "env") impl.env.serialize(section.payload);
    else if (name == "command") impl.command.serialize(section.payload);
    else if (name == "match") impl.match.serialize(section.payload);
    else if (name == "ai") impl.ai.serialize(section.payload);
    else if (name == "campaign") impl.campaign.serialize(section.payload);
    else if (name == "areas") impl.areas.serialize(section.payload);
    else if (name == "fog") impl.fog.serialize(section.payload);
    // A name in `kSystemOrder` with no system behind it is a wiring mistake,
    // the same one `create` refuses.
    else return FormatError::malformed;
    sections.push_back(std::move(section));
  }

  // The session's own section: the script watermark. Self-describing like a
  // system's section -- magic, version, then the one field -- so that the
  // loader can tell a truncated section from an empty one.
  std::vector<std::byte> session;
  bytes::put_u32(session, kSessionMagic);
  bytes::put_u32(session, kSessionSectionVersion);
  bytes::put_u32(session, impl.script_watermark);
  // The passability layer the grid was built from, when `start_match`
  // rebuilt it: the grid is not a function of the map alone once the
  // templates have landed, and a load that adopted the shipped layer would
  // route the restored side through the towns the running one walks
  // around. Written as the `GRID` container it is, so it parses like a
  // layer; absent when the map's own layer is in use.
  if (impl.passability.rebuilt && impl.pass_layer.grid().cell_size() != 0) {
    const std::vector<std::byte> layer = write_grid(impl.pass_layer.grid());
    bytes::put_u32(session, static_cast<std::uint32_t>(layer.size()));
    session.insert(session.end(), layer.begin(), layer.end());
  } else {
    bytes::put_u32(session, 0);
  }
  // The templates laid, so that a load lays their ground again: the layers
  // a session plays on are the map's plus that ground, and nothing else
  // records which templates the draw chose.
  bytes::put_u32(session, static_cast<std::uint32_t>(impl.materialised.stamps.size()));
  for (const TemplateStamp& stamp : impl.materialised.stamps) {
    bytes::put_u32(session, static_cast<std::uint32_t>(stamp.index));
    bytes::put_i32(session, stamp.origin.x);
    bytes::put_i32(session, stamp.origin.y);
  }
  // The AI's nodes as they stand, which after a town hall has fallen is not
  // what a rebuild over the restored world would make.
  const std::span<const GaikaNode> nodes = impl.world.gaika().nodes();
  bytes::put_u32(session, static_cast<std::uint32_t>(nodes.size()));
  for (const GaikaNode& node : nodes) {
    bytes::put_i32(session, node.center.x);
    bytes::put_i32(session, node.center.y);
    bytes::put_i32(session, node.lsa);
    bytes::put_u32(session, node.settlement);
  }

  SaveInputs inputs;
  inputs.scheduler = &impl.scheduler;
  inputs.selections = &impl.selections;
  inputs.systems = sections;
  inputs.session = session;
  inputs.map = map;

  std::vector<std::byte> out;
  const Result<SaveReport> report = write_save(impl.world, inputs, out);
  if (!report.ok()) return report.error();
  // Every registered system has a section above, so this is a wiring assertion
  // rather than a runtime condition: if it ever fires, `kSystemOrder` and the
  // dispatch have drifted apart.
  if (!report->unsaved_systems.empty()) return FormatError::malformed;
  return out;
}

/// Every `.vs` path a restored session needs in its library, in a fixed order.
///
/// **Two halves, and they answer two different questions.**
///
/// The first is *what was running*, and it comes from the save's own script
/// section. This function used to enumerate the ways a script can start
/// instead -- the victory condition, the AI profile's `[Scripts]` manifest,
/// each class's `idle` binding, and later each class's hooks -- and the
/// enumeration was wrong every time somebody added a way to start a script.
/// It was wrong for `CommandSystem::launch` from the day that function learned
/// to compile through the library, which is why `Balcans` and the conquest
/// failed every save round trip on `deer_move`, `eagle_move`, `lion_lead` and
/// `wolf_lead`: four wildlife verbs, launched by the command pump, named by no
/// manifest anywhere. A list of what *can* happen is a claim that has to be
/// maintained; a list of what *did* is a fact.
///
/// The second is *what can still be looked up*, and no save can answer it.
/// `AIRun` and `StartPlayerScript` reach `Scheduler::spawn_by_name`, which is
/// a lookup in the loaded library and compiles nothing -- so a restored AI
/// script that calls `AIRun('SquadMonitor.vs')` on its next slice needs that
/// file present whether or not anything was running it when the save was
/// taken. That is what the profile's `[Scripts]` manifest below is for. The
/// victory condition is beside it because `MatchSystem` may start one after
/// the load.
///
/// Everything else -- class `idle` methods, class hooks, command verbs, AI
/// helpers -- reaches the library through `chunk_for` and compiles on demand,
/// so it needs no manifest at all. The idle walk is kept for one reason and it
/// is named rather than assumed: `start_object_scripts` runs at the top of the
/// next turn over every object at once, and compiling its scripts here spreads
/// that cost over the load instead of the first tick.
///
/// **The order does not have to match the writer's** -- the scheduler resolves
/// by name and rewrites each coroutine's `chunk_index` from the lookup -- but
/// it is fixed all the same, because a chunk index that moved between two loads
/// of one save would make the two sessions differ in a way nothing hashes.
void GameSession::Impl::prime_library(std::span<const std::byte> script_section) {
  if (scripts == nullptr) return;

  // **What the save itself names, first and exactly.** Everything below this is
  // a manifest of what *can be started*; this is the list of what *was
  // running*, written by the writer, and it is the only one that cannot be
  // short. A malformed section is left to `Scheduler::deserialize` to reject
  // with its own error rather than pre-empted here.
  if (const Result<std::vector<std::string>> running =
          script::script_section_sources(script_section);
      running.ok()) {
    for (const std::string& path : running.value()) (void)chunk_for(path);
  }

  // The victory condition, from the rules the match section just restored --
  // not from `game.xml`, because a script can have changed them since.
  const std::string victory = victory_script_path(match.rules().condition);
  if (!victory.empty()) (void)chunk_for(victory);

  for (const AiScriptDeclaration& declaration : profile.scripts()) {
    (void)chunk_for("DATA/AI/" + declaration.file);
  }

  if (classes == nullptr) return;
  // Object order, which is ascending id order. A class's `idle` is compiled
  // once however many objects bind it; `chunk_for` memoises.
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.class_index == kNoClass) continue;
    for (const std::string_view path : classes->resolved_behaviors(slot.class_index)) {
      (void)chunk_for(path);
    }
    for (const ClassMethod& method : classes->resolved_methods(slot.class_index)) {
      if (method.sig != kIdleMethod) continue;
      (void)chunk_for(method.vs);
      break;
    }
  }
}

Status GameSession::load(std::span<const std::byte> bytes, std::string_view map,
                         SessionLoadReport* out) {
  Impl& impl = *impl_;

  const Result<SaveReader> reader = SaveReader::open(bytes);
  if (!reader.ok()) return reader.error();

  LoadOptions options;
  options.entities = impl.entities;
  options.selections = &impl.selections;
  // **Deliberately null.** `read_save` would apply the script section here,
  // before the library exists. It is applied below instead, and the section is
  // struck off the unconsumed list by hand.
  options.scheduler = nullptr;
  options.map = map;
  options.require_same_pipeline = true;

  const Result<LoadReport> report = read_save(reader.value(), impl.world, options);
  if (!report.ok()) return report.error();

  SessionLoadReport summary;
  summary.meta = report->meta;
  summary.unrestored_systems = report->unrestored_systems;
  summary.objects = report->objects;
  for (const std::string& name : report->unconsumed) {
    if (name == kScriptSection || name == kSessionSection) continue;
    summary.unconsumed.push_back(name);
  }

  // The session's own section, before any turn can run the sweep. Required,
  // not optional: every save this build writes has it, and a load without it
  // would leave the watermark where `start_object_scripts` on this fresh
  // session put it -- the map's `next_id()` -- so that the first turn after
  // the load re-offered `idle` to every object spawned since the match began.
  // That is a different game, refused as such.
  std::vector<GaikaNode> saved_nodes;
  {
    const std::span<const std::byte> payload = reader->section(kSessionSection);
    if (!reader->has(kSessionSection)) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
    ByteReader section(payload);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t watermark = 0;
    std::uint32_t layer_size = 0;
    if (!section.u32(magic) || !section.u32(version) || !section.u32(watermark) ||
        magic != kSessionMagic || version != kSessionSectionVersion || !section.u32(layer_size) ||
        section.remaining() < layer_size) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
    const std::span<const std::byte> layer = payload.subspan(payload.size() - section.remaining(), layer_size);
    (void)section.skip(layer_size);
    // The templates laid, and their ground laid again over the map's layers
    // before anything reads them.
    std::uint32_t stamp_count = 0;
    std::vector<TemplateStamp> stamps;
    if (!section.u32(stamp_count) || stamp_count > 4096) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
    for (std::uint32_t i = 0; i < stamp_count; ++i) {
      TemplateStamp stamp;
      std::uint32_t index = 0;
      if (!section.u32(index) || !bytes::get_i32(section, stamp.origin.x) || !bytes::get_i32(section, stamp.origin.y)) {
        if (out != nullptr) *out = std::move(summary);
        return Status(FormatError::malformed);
      }
      stamp.index = index;
      stamps.push_back(stamp);
    }
    // The AI's nodes, adopted once the areas they name are rebuilt below. A
    // count the bytes cannot hold is refused before anything is reserved for
    // it; the reads below would refuse it too, after the reserve, so this
    // guard changes what is allocated and not what is answered.
    std::uint32_t node_count = 0;
    if (!section.u32(node_count) || node_count > section.remaining() / kSavedGaikaNodeBytes) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
    saved_nodes.reserve(node_count);
    for (std::uint32_t i = 0; i < node_count; ++i) {
      GaikaNode node;
      if (!bytes::get_i32(section, node.center.x) || !bytes::get_i32(section, node.center.y) ||
          !bytes::get_i32(section, node.lsa) || !section.u32(node.settlement)) {
        if (out != nullptr) *out = std::move(summary);
        return Status(FormatError::malformed);
      }
      saved_nodes.push_back(node);
    }
    if (section.remaining() != 0) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
    impl.ground = GroundReport{};
    impl.lay_template_ground(stamps);
    impl.materialised.stamps = std::move(stamps);
    // The layer the saved grid was built from, ahead of the systems: the
    // movement section carries the grid's generation, and a grid set after
    // it would bump the generation and send every restored route back to
    // the pathfinder.
    impl.passability = PassabilityReport{};
    if (layer_size != 0) {
      const auto refuse = [&]() {
        if (out != nullptr) *out = std::move(summary);
        return Status(FormatError::malformed);
      };
      const Result<Grid> grid = Grid::parse(layer);
      if (!grid.ok() || !grid->validate(layer.size()).ok()) return refuse();
      Result<OwnedGrid> owned = OwnedGrid::copy(grid.value());
      Result<ObstructionGrid> obstruction = ObstructionGrid::from_grid(grid.value());
      if (!owned.ok() || !obstruction.ok()) return refuse();
      impl.pass_layer = std::move(owned.value());
      impl.movement.set_grid(std::move(obstruction.value()));
      impl.passability.rebuilt = true;
    }
    // A watermark past the ids the world has handed out names objects that do
    // not exist; nothing this build writes can produce one.
    if (watermark > impl.world.next_id()) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
    impl.script_watermark = watermark;
    // And whether the sweep runs at all, which is the saved game's and not
    // this session's: a watermark is set only by a `start_object_scripts`
    // that had scripts to start, so a saved one means the saved game gave
    // every object spawned since its `idle`, and this one must go on doing
    // so. Taken from the session instead, a joiner loaded into a session
    // nobody had started -- which this header says loads correctly -- never
    // gave a trained unit its `idle`, and parted from its peers a few turns
    // after a save taken mid-war (`imconform netjoin --skirmish`). The
    // converse holds too: a save of a game that never ran object scripts
    // does not start running them because the loading session had.
    impl.object_scripts_wanted = watermark != kNoObject;
  }

  // System sections, in `kSystemOrder`. Before the scripts, because a
  // coroutine's own state is meaningless without the systems it was suspended
  // observing -- and because `prime_library` reads `MatchSystem::rules`.
  for (const std::string_view name : kSystemOrder) {
    const std::span<const std::byte> payload = reader->system_section(name);
    if (payload.empty()) continue;  // reported through `unrestored_systems`
    Status status;
    if (name == "movement") status = impl.movement.deserialize(payload);
    else if (name == "combat") status = impl.combat.deserialize(payload);
    else if (name == "economy") status = impl.economy.deserialize(payload);
    else if (name == "feeder") status = impl.feeder.deserialize(payload);
    else if (name == "hero") status = impl.hero.deserialize(payload);
    else if (name == "env") status = impl.env.deserialize(payload);
    else if (name == "command") status = impl.command.deserialize(payload);
    else if (name == "match") status = impl.match.deserialize(payload);
    else if (name == "ai") status = impl.ai.deserialize(payload);
    else if (name == "campaign") status = impl.campaign.deserialize(payload);
    else if (name == "areas") status = impl.areas.deserialize(payload);
    else if (name == "fog") status = impl.fog.deserialize(payload);
    else status = FormatError::malformed;
    if (!status.ok()) {
      if (out != nullptr) *out = std::move(summary);
      return status;
    }
  }

  // The two tables `create` built over the map's own settlement list, which a
  // skirmish map's is not: `start_match` made every `Mutable` placeholder into
  // a race's town and rebuilt both over the new anchors, and a load that kept
  // the placeholders' tables routed every unit on the restored side around a
  // stronghold that no longer stood there -- the save sweep's divergence on
  // Balcans and Crossroads, four turns after the load, in a freshly trained
  // unit's path.
  //
  // The areas are a function of the terrain and the grid, both restored by
  // now, and are rebuilt. **The nodes are not rebuilt but restored**: a node
  // keeps the centre its town hall had when the table was built, and once
  // that town hall has been razed no rebuild can find it. `GaikaTable::restore`
  // says what rebuilding them cost.
  impl.world.mutable_lsa().build(impl.world.terrain(), impl.movement.grid());
  for (const GaikaNode& node : saved_nodes) {
    if (node.lsa != kNoLsa && (node.lsa < 1 || static_cast<std::size_t>(node.lsa) > impl.world.lsa().size())) {
      if (out != nullptr) *out = std::move(summary);
      return Status(FormatError::malformed);
    }
  }
  impl.world.mutable_gaika().restore(std::move(saved_nodes), impl.world.lsa());

  if (reader->has(kScriptSection)) {
    impl.prime_library(reader->section(kScriptSection));
    const Status status = impl.scheduler.deserialize(reader->section(kScriptSection));
    if (!status.ok()) {
      if (out != nullptr) *out = std::move(summary);
      return status;
    }
  }
  summary.scripts = impl.scheduler.live_count();

  // Report bookkeeping, so that `report()` after a load describes the game that
  // was loaded rather than the one this process happened to run. `started` and
  // `finished` are counters of this process's own activity and are reset for
  // the same reason: a session that has loaded somebody else's game has started
  // nothing.
  impl.turns = impl.world.turns();
  impl.started = summary.scripts;
  impl.finished = 0;
  impl.traps.clear();

  summary.hashes = verify_hashes(impl.world, summary.meta);
  const bool ok = summary.hashes.ok;
  if (out != nullptr) *out = std::move(summary);
  // A mismatch means a section did not come back, and the session is now a game
  // that never existed. Refused rather than returned, because the alternative
  // is a divergence twenty turns from here with nothing to point at.
  return ok ? Status() : Status(FormatError::malformed);
}

}  // namespace imperivm::core::sim
