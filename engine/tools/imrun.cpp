// Run a shipped map headlessly and report what the simulation still needs.
//
// This is the falsifier for the coverage number. `docs/plan.html` says 257 host
// entry points are implemented, covering 77% of the corpus's call sites, and
// that figure is arithmetic over a static inventory: it counts what exists, not
// what works. Every one of this project's worst bugs -- a `KillCommand` that
// destroyed its receiver, one `void*` meaning three types, a query returning
// the caller's own objects instead of its enemies' -- lived in code the
// inventory counted as implemented, and was invisible until two domains met.
//
// So this loads a retail map, starts every object's own `idle` script, runs the
// simulation, and prints the traps grouped by cause. A trap is not a failure
// here; it is the measurement. The output is deterministic and diffable, so two
// runs of one build agree and a change that moves the needle says so.
//
// It lives outside engine/core because it opens files, which core may not.

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>

#include "imperivm/gamedata/installation.hpp"
#include "imperivm/gamedata/campaign_file.hpp"
#include "imperivm/gamedata/save_file.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"

using namespace imperivm::core;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: imrun <game-dir> <map.bfhp> [turns] [turn-length-ms] [map-number]\n"
                 "       imrun <game-dir> --load <save.bfhp> [turns] [turn-length-ms]\n"
                 "\nRuns a map headlessly and reports the traps, most-hit first.\n"
                 "A conquest holds several maps; `map-number` picks one by its\n"
                 "`Maps/<n>` directory number, which is not an index -- the shipped\n"
                 "conquest numbers its seven maps 3, 4, 6, 7, 8, 9, 10. Without it\n"
                 "the first in walk order is used.\n"
                 "\n`--load` resumes a save file -- the container `imsave --out` and\n"
                 "the app write -- on the map its manifest names, and runs on from\n"
                 "the turn it was saved at.\n"
                 "\nTrailing options, after the positionals:\n"
                 "  --campaign FILE   the campaign between missions: restore FILE's\n"
                 "                    carry before the mission's sequences start, and\n"
                 "                    write FILE when the human's match ends won\n"
                 "  --human N         which slot the human holds (default: none)\n"
                 "  --declare-won     end the human's match as won after the run, as a\n"
                 "                    victory sequence would, so that the carry is\n"
                 "                    written; a knob for testing the boundary, and\n"
                 "                    said so in the output\n"
                 "\nIMRUN_UNTIL_OVER=1 stops at the turn the match is decided; the\n"
                 "`match` block says which turn that was either way.\n"
                 "IMRUN_DEATHS=1 prints every death, where and to whom;\n"
                 "IMRUN_OBJECTS=<text>[,<text>...] lists every object whose class\n"
                 "contains any of them;\n"
                 "IMRUN_OVERLAPS=<n> counts standing bodies drawn through each other\n"
                 "every n turns and prints the worst turn, and the melee units in a\n"
                 "fight that are engaged, closing or waiting out of reach; with\n"
                 "IMRUN_PILES=1 as well, each sample also lists every 16-unit cell\n"
                 "three or more standing bodies share, by class, owner, order and\n"
                 "the hero whose army it is (h<id>, or h- for none);\n"
                 "IMRUN_GOTO=<turn>:<id>:<x>,<y> gives that object its owner's\n"
                 "right-click order to the point before that turn, IMRUN_PLACE=<x>,<y>\n"
                 "first stands it there, out of its AI's hands, IMRUN_WATCH=<id>\n"
                 "prints where it stands every ten turns and every gate its route\n"
                 "crosses, and IMRUN_GATES=1 prints each gate as it opens or closes\n"
                 "and as it starts or stops letting units through, and every gate\n"
                 "at the end.\n");
    return 2;
  }
  const std::string game = argv[1];
  std::string map = argv[2];
  std::string map_index;
  std::uint32_t seed = 1;
  // `--load FILE` takes the map, the map number and the seed from the file's
  // manifest; the session is then built exactly as for a fresh run and the
  // save applied over it, which is the one flow `GameSession::load` supports.
  sim::SaveFileContents resume;
  bool resuming = false;
  int positional = 3;
  if (map == "--load") {
    if (argc < 4) {
      std::fprintf(stderr, "--load needs a file\n");
      return 2;
    }
    std::string error;
    if (!imperivm::gamedata::read_save_file(argv[3], resume, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    map = imperivm::gamedata::container_absolute(game, resume.manifest.container).string();
    map_index = resume.manifest.map_index;
    seed = resume.manifest.seed;
    resuming = true;
    positional = 4;
  }
  // Positionals up to the first `--` option; the options after them.
  std::string campaign_path;
  sim::PlayerId human = kNoPlayer;
  bool declare_won = false;
  int end = positional;
  while (end < argc && std::strncmp(argv[end], "--", 2) != 0) ++end;
  for (int i = end; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--campaign" && i + 1 < argc) campaign_path = argv[++i];
    else if (flag == "--human" && i + 1 < argc) human = static_cast<sim::PlayerId>(std::atoi(argv[++i]));
    else if (flag == "--declare-won") declare_won = true;
    else {
      std::fprintf(stderr, "unknown option %s\n", flag.c_str());
      return 2;
    }
  }
  const std::uint64_t turns =
      end > positional ? std::strtoull(argv[positional], nullptr, 10) : 200;
  const std::int32_t length = end > positional + 1 ? std::atoi(argv[positional + 1]) : 800;
  if (!resuming && end > positional + 2) map_index = argv[positional + 2];

  // Loading an installation is `imperivm_gamedata`'s job, not this tool's. It
  // was this tool's for a while, and then the windowed application needed the
  // same four things -- a pack reader, a class-graph builder, a container
  // loader and a script resolver -- which is when it moved.
  imperivm::gamedata::Installation install;
  std::string error;
  if (!install.open(game, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }

  imperivm::gamedata::MapContainer container;
  if (!container.open(map, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  const imperivm::gamedata::MapPayloads payloads =
      imperivm::gamedata::read_payloads(container, map_index);
  if (!payloads.ok()) {
    std::fprintf(stderr, "no map.obj.xml in %s\n", map.c_str());
    return 1;
  }

  script::HostRegistry registry;
  script::HostRegistry inventory;
  script::declare_shipped_surface(inventory);
  sim::register_all_hosts(registry);

  // Coverage is reported against the **shipped inventory**, not against the
  // assembled registry. `HostRegistry::define` introduces a name that was not
  // declared, and twelve entries take advantage of that: the scheduler
  // registers `AIRun` at every arity from 1 to 5 because it is variadic, six
  // ambient command-context names are implemented as zero-arity free functions
  // because that is how a bare identifier resolves, and `GetItemIndex` is
  // reachable only from an inline `script="..."` in `DATA/ITEMS.XML`. All
  // defensible; all of them would inflate a figure meant to say how much of the
  // corpus can run.
  std::size_t covered = 0;
  std::size_t extra = 0;
  for (const script::HostEntry& entry : registry.entries()) {
    if (entry.fn == nullptr) continue;
    if (inventory.find(entry.kind, entry.name, entry.arity) == script::kUnresolvedHost) {
      ++extra;
    } else {
      ++covered;
    }
  }

  sim::SessionInputs inputs = imperivm::gamedata::session_inputs(install, payloads);

  // A container's own scripts are in no pack. Without this chain every
  // `Sequences/seq*.vs` fails to resolve and the campaign layer never runs --
  // which is exactly how it looked for as long as this tool ran only `idle`
  // behaviours out of `data.pak` and reported six traps on a map with 1,119
  // scripts and no mission.
  imperivm::gamedata::ContainerScripts scripts(container, install.scripts());
  inputs.scripts = &scripts;

  auto session = sim::GameSession::create(registry, inputs, seed);
  if (!session.ok()) {
    // `create` folds a dozen optional payloads into one error code, and "3"
    // over seven conquest maps says nothing about which document is at fault.
    // Re-parse each input on its own and name the one that refused.
    std::fprintf(stderr, "session failed to build: %d\n", static_cast<int>(session.error()));
    const auto report = [](const char* what, FormatError error) {
      std::fprintf(stderr, "  %-14s %s\n", what,
                   error == FormatError::none ? "ok" : "REFUSED");
    };
    if (!inputs.map_objects.empty()) {
      const auto parsed = MapObjectList::parse(inputs.map_objects);
      report("map.obj.xml", parsed.ok() ? FormatError::none : parsed.error());
    }
    if (!inputs.conquest.empty()) {
      const auto parsed = sim::ConquestMap::parse(inputs.conquest);
      report("territories.xml", parsed.ok() ? FormatError::none : parsed.error());
    }
    if (!inputs.player_setup.empty()) {
      sim::PlayerTable table;
      const Status loaded = load_player_table(inputs.player_setup, table);
      report("player*.xml", loaded ? FormatError::none : loaded.error());
      for (std::size_t i = 0; i < inputs.player_setup.size(); ++i) {
        if (inputs.player_setup[i].empty()) {
          std::fprintf(stderr, "  player%zu.xml     MISSING\n", i);
        }
      }
      for (std::size_t i = 0; i < inputs.player_setup.size(); ++i) {
        if (inputs.player_setup[i].empty()) continue;
        sim::PlayerTable one_table;
        const std::span<const std::byte> one[] = {inputs.player_setup[i]};
        if (!load_player_table(one, one_table)) {
          std::fprintf(stderr, "  player%zu.xml     REFUSED\n", i);
        }
      }
    }
    {
      const auto ini = IniDocument::parse(inputs.constants);
      report("CONST.INI", ini.ok() ? FormatError::none : ini.error());
      const auto profile = sim::AiProfile::parse(inputs.ai_profile);
      report("AI.INI", profile.ok() ? FormatError::none : profile.error());
    }
    return 1;
  }
  sim::GameSession& run = *session.value();

  std::printf("classes   %zu from %zu files\n", install.classes().size(), install.class_files());
  std::printf("objects   %zu\n", run.world().objects().size());
  // Trigger regions attached to a named object. Printed because a subsystem
  // that is complete and *unreachable* has cost this project twice: the
  // settlement timers and the command table each passed every unit test while
  // nothing ran the data's own path. Zero on a map that authored none, and
  // zero on a map whose areas failed to bind -- which is the case worth seeing.
  if (const sim::AreaSystem* areas = sim::area_system_of(run.world())) {
    std::printf("areas     %zu named\n", areas->areas().size());
  }
  // The mission's own objectives, declared and given. Printed for the same
  // reason the area count is, and with the same failure in view: a container
  // whose `Notes.xml` never reached the session declares zero and gives zero,
  // and `GiveNote` is silent about it by design -- a note nothing declares
  // cannot be given, so an unloaded catalogue and a mission that pins nothing
  // look identical from the script side.
  if (const sim::CampaignSystem* campaign = sim::campaign_system_of(run.world())) {
    std::printf("notes     %zu declared\n", campaign->notes().size());
  }
  // Who can fight, and over what ground. Printed for the same reason the area
  // count is: `CombatSystem` and `MovementSystem::set_grid` were both complete,
  // tested, registered and unreachable, and this is the only number that could
  // have said so. Zero combatants on a map with two armies on it is a failure,
  // not a pass.
  if (const sim::CombatSystem* fight = sim::combat_system_of(run.world())) {
    std::size_t attackers = 0;
    for (const sim::Combatant& unit : fight->combatants()) {
      if (fight->profile(unit.class_index).can_attack()) ++attackers;
    }
    std::printf("combat    %zu enrolled, %zu of them can attack\n", fight->combatants().size(),
                attackers);
  }
  if (const sim::MovementSystem* move = sim::movement_system(run.world())) {
    const sim::ObstructionGrid& grid = move->grid();
    if (grid.empty()) {
      std::printf("terrain   NO OBSTRUCTION GRID -- units path through walls and water\n");
    } else {
      const std::size_t cells = static_cast<std::size_t>(grid.width()) * grid.height();
      std::printf("terrain   %d x %d cells of %d units, %zu blocked (%zu%%)\n", grid.width(),
                  grid.height(), sim::kCollisionCellSize, grid.count_blocked(),
                  cells == 0 ? 0 : grid.count_blocked() * 100 / cells);
    }
  }
  std::printf("host      %zu of %zu shipped entry points implemented, plus %zu outside it\n",
              covered, inventory.size(), extra);
  // Registration order is run order and is folded into the world hash, so a
  // reader comparing two runs needs to see it.
  std::printf("systems  ");
  for (const sim::System* system : run.world().systems()) {
    std::printf(" %.*s", static_cast<int>(system->name().size()), system->name().data());
  }
  std::printf("\n");

  // The victory condition from `game.xml`, which is the other half of the
  // campaign layer: `MatchSystem` owns `EndGame`, and the shipped victory
  // scripts are what call it when no sequence does.
  sim::MatchOptions match_options;
  match_options.human = human;
  const std::size_t victory_started = run.start_match(match_options);
  const std::size_t objects_started = run.start_object_scripts();
  // The AI, for every computer-controlled player -- the app's order: match,
  // objects, AI. **This tool ran without it for as long as it existed**, so
  // every trap tally quoted from it measured a game in which no `Main.vs` ever
  // started and no army was ever raised, and a coverage figure that says
  // `DATA\AI` runs was never once checked against a run of `DATA\AI`.
  const std::size_t ai_started = run.start_ai();
  // `IMRUN_AILOG=1`: the shipped AI's own narration. `AI.INI` declares
  // `AIV_LogPlayer`, a player mask, and fourteen `AIV_Log*` switches --
  // `AIV_LogRecruit`, `AIV_LogArmy`, `AIV_LogFlee` -- that every `DATA\AI`
  // script tests before it `pr`s what it is deciding. All off in the shipped
  // profile. Turned on here for every player, with `pr` routed to stdout,
  // the AI explains itself line by line, which is how "fifteen AIs, no army"
  // becomes a sentence naming the script that is waiting and for what.
  struct StdoutSink final : sim::DebugSink {
    void write(std::string_view text) override {
      std::printf("  pr  %.*s\n", static_cast<int>(text.size()), text.data());
    }
  };
  static StdoutSink sink;
  if (std::getenv("IMRUN_AILOG") != nullptr) {
    run.set_debug_sink(&sink);
    if (sim::EnvSystem* env = sim::env_of(run.world())) {
      const std::int32_t mask = env->ai_var_id("AIV_LogPlayer");
      for (std::int32_t player = 1; player <= 16; ++player) {
        if (mask >= 0) (void)env->ai_vars().set(player, mask, 0xffff);
        for (const char* name :
             {"AIV_LogTrain", "AIV_LogSiege", "AIV_LogRecruit", "AIV_LogResearch", "AIV_LogAUF",
              "AIV_LogHeroes", "AIV_LogArmy", "AIV_LogFoodSell", "AIV_LogGetGS",
              "AIV_LogFreezeArmy", "AIV_LogFlee", "AIV_LogFeed", "AIV_LogRush"}) {
          const std::int32_t id = env->ai_var_id(name);
          if (id >= 0) (void)env->ai_vars().set(player, id, 1);
        }
      }
    }
  }
  // `IMRUN_TRACE=<text>`: every host call made by a script whose source
  // name contains `<text>`, with its arguments, its outcome and its line --
  // the question "where does ES_Stronghold.vs stop" answered without editing
  // the script. `IMRUN_TRACE_LIMIT` caps the lines (default 2000).
  struct Trace {
    script::Scheduler* scheduler = nullptr;
    std::string needle;
    std::size_t limit = 2000;
    std::size_t printed = 0;
  };
  static Trace trace;
  if (const char* needle = std::getenv("IMRUN_TRACE")) {
    trace.scheduler = &run.scheduler();
    trace.needle = needle;
    if (const char* limit = std::getenv("IMRUN_TRACE_LIMIT")) trace.limit = std::strtoul(limit, nullptr, 10);
    run.scheduler().set_call_trace(
        [](void* user, script::ScriptId, std::string_view source_name, std::string_view name,
           std::span<const script::Value> args, const script::HostOutcome& outcome,
           std::int32_t line) {
          Trace& t = *static_cast<Trace*>(user);
          if (t.printed >= t.limit) return;
          const std::string source(source_name);
          if (source.find(t.needle) == std::string::npos) return;
          ++t.printed;
          std::string text = "  tr  " + source + ":" + std::to_string(line) + " " +
                             std::string(name) + "(";
          const auto show = [&](const script::Value& v) {
            if (v.is_integer()) return std::to_string(v.as_integer());
            if (v.is_string()) return "\"" + v.as_string() + "\"";
            if (v.is_nil()) return std::string("nil");
            script::Host* host = t.scheduler->host();
            if (host == nullptr) return std::string("?");
            const auto rendered = host->to_string(v);
            return rendered.ok() ? rendered.value() : std::string("?");
          };
          for (std::size_t i = 0; i < args.size(); ++i) {
            if (i > 0) text += ", ";
            text += show(args[i]);
          }
          text += ")";
          switch (outcome.status) {
            case script::HostStatus::ok:
              text += " -> " + (outcome.value.is_nil() ? std::string("void") : show(outcome.value));
              break;
            case script::HostStatus::retry: text += " -> retry"; break;
            case script::HostStatus::suspend: text += " -> suspend"; break;
            case script::HostStatus::finish: text += " -> finish"; break;
            case script::HostStatus::error:
              text += std::string(" -> ERROR ") + (outcome.error != nullptr ? outcome.error : "");
              break;
            default: text += " -> ?"; break;
          }
          std::printf("%s\n", text.c_str());
        },
        &trace);
  }
  // The campaign between missions, before the sequences: the conquest's root
  // sequence reads `ConquestBonus()` on its first pass. A file for another
  // conquest -- or none -- is reported and leaves the authored start.
  const std::string container_relative = imperivm::gamedata::container_relative(game, map);
  if (!campaign_path.empty()) {
    sim::CampaignCarry carry;
    std::string why;
    if (!imperivm::gamedata::read_campaign_file(campaign_path, carry, &why)) {
      std::printf("carry     %s\n", why.empty() ? "no campaign in progress" : why.c_str());
    } else if (carry.container != container_relative) {
      std::printf("carry     %s is a campaign of %s, not of this container\n",
                  campaign_path.c_str(), carry.container.c_str());
    } else if (const Status status = run.restore_campaign(carry); !status.ok()) {
      std::printf("carry     %s refused by this conquest's table (error %d)\n",
                  campaign_path.c_str(), static_cast<int>(status.error()));
    } else {
      std::printf("carry     %s restored: %zu conquered, bonus \"%s\"\n", campaign_path.c_str(),
                  carry.progress.conquered.size(), carry.progress.active_bonus.c_str());
    }
  }

  // The campaign layer, in the order the retail engine reaches it: the
  // container's own manifest first, then the map's.
  const std::size_t game_started =
      run.start_sequences(payloads.game_sequences, /*base=*/"");
  const std::size_t map_started =
      run.start_sequences(payloads.map_sequences, payloads.map_directory);

  std::printf("map       %s\n",
              payloads.map_directory.empty() ? "(root)" : payloads.map_directory.c_str());
  std::printf("campaign  %s",
              payloads.conquest.empty() ? "no territories.xml" : "territories.xml loaded");
  if (const sim::CampaignSystem* campaign = sim::campaign_system_of(run.world())) {
    if (!campaign->territory_ids().empty()) {
      std::printf(", %zu territories, bonus \"%.*s\"", campaign->territory_ids().size(),
                  static_cast<int>(campaign->active_bonus().size()),
                  campaign->active_bonus().data());
    }
  }
  std::printf("\n");
  std::printf("victory   %zu script(s) started\n", victory_started);
  {
    const sim::MaterialiseReport& made = run.materialised();
    std::printf("mutable   %zu stronghold(s) and %zu village(s) made into a race's, %zu left\n",
                made.strongholds, made.villages, made.left);
    // The layer rebuilt over the world as it stands, the original's
    // 0x00552e95: the cells that differ from the shipped layer are the
    // templates' footprints, and on an authored map there are none.
    const sim::GroundReport& ground = run.ground();
    if (!ground.changed.empty()) {
      std::printf("ground    %zu template(s) laid their ground: %zu cells copied, %zu height cells levelled, "
                  "%zu sloped, %zu decorations bulldozed, %zu shore cells\n",
                  ground.changed.size(), ground.cells_copied, ground.cells_levelled, ground.cells_sloped,
                  ground.decorations_bulldozed, ground.shore_cells);
    }
    const sim::PassabilityReport& pass = run.passability();
    if (pass.rebuilt) {
      std::printf("pass      layer rebuilt at match start, %zu cell(s) differ from the shipped one\n",
                  pass.cells_changed);
    } else {
      std::printf("pass      layer adopted as shipped\n");
    }
  }
  std::printf("sequences %zu declared (%zu game, %zu map), %zu autorun started\n",
              payloads.game_sequences.size() + payloads.map_sequences.size(),
              payloads.game_sequences.size(), payloads.map_sequences.size(),
              game_started + map_started);
  std::printf("ai        %zu player(s) started\n", ai_started);
  std::printf("scripts   %zu started (%zu object idle, %zu sequence, %zu victory, %zu ai)\n",
              objects_started + game_started + map_started + victory_started + ai_started,
              objects_started, game_started + map_started, victory_started, ai_started);

  if (resuming) {
    // Over the started session, as the app does it: the saved coroutines
    // replace what was just started, and the load refuses -- rather than
    // runs -- a save whose hashes do not come back.
    sim::SessionLoadReport loaded;
    const Status status =
        run.load(resume.session, imperivm::gamedata::map_identity(resume.manifest), &loaded);
    if (!status.ok()) {
      if (!loaded.hashes.ok) {
        std::fprintf(stderr, "load refused: channel %.*s expected %016llx actual %016llx\n",
                     static_cast<int>(loaded.hashes.channel.size()), loaded.hashes.channel.data(),
                     static_cast<unsigned long long>(loaded.hashes.expected),
                     static_cast<unsigned long long>(loaded.hashes.actual));
      } else if (const std::string why = sim::save_version_refusal(resume.session); !why.empty()) {
        std::fprintf(stderr, "load refused: %s\n", why.c_str());
      } else {
        std::fprintf(stderr, "load refused: error %d\n", static_cast<int>(status.error()));
      }
      return 1;
    }
    std::printf("loaded    %s: %zu objects, %zu scripts, turn %llu, time %lld, hashes verified\n",
                argv[3], loaded.objects, loaded.scripts,
                static_cast<unsigned long long>(loaded.meta.turns),
                static_cast<long long>(loaded.meta.time));
    for (const std::string& name : loaded.unrestored_systems) {
      std::printf("          UNRESTORED %s\n", name.c_str());
    }
    for (const std::string& name : loaded.unconsumed) {
      std::printf("          UNCONSUMED %s\n", name.c_str());
    }
  }
  std::printf("\n");

  // The combat baseline, taken before the first turn. "Nobody died" and "the
  // system never ran" look identical from the far end, so both the population
  // and the total health are recorded here and differenced below: health that
  // has not moved means no damage was dealt, whatever the corpse count says.
  std::size_t combat_before = 0;
  std::int64_t health_before = 0;
  if (const sim::CombatSystem* fight = sim::combat_system_of(run.world())) {
    combat_before = fight->combatants().size();
    for (const sim::Combatant& unit : fight->combatants()) health_before += unit.health;
  }

  // Turn by turn rather than `advance(turns, length)`, which is the same loop:
  // `CombatSystem` clears its event list at the top of every `advance`, so the
  // strikes have to be drained as they happen. This is the measurement that
  // distinguishes "combat ran and nothing was in reach" from "combat never
  // ran", and the object count cannot do it -- a script evaluating a query
  // spawns and despawns handle-bearing query objects every turn, so the world's
  // object count moves for reasons that have nothing to do with anybody dying.
  std::size_t strikes = 0;
  std::size_t launches = 0;
  std::size_t impacts = 0;
  std::size_t deaths = 0;
  std::int64_t dealt = 0;
  // The turn the match was decided, and -- with `IMRUN_UNTIL_OVER` set -- the
  // last one run. A skirmish that ends by itself does so at no turn anybody
  // can name in advance, so a caller asking "does it end" gives a generous
  // `turns` and this stops at the answer rather than running out the rest.
  const bool until_over = std::getenv("IMRUN_UNTIL_OVER") != nullptr;
  const bool show_deaths = std::getenv("IMRUN_DEATHS") != nullptr;
  // A death event names no attacker; the last blow that landed on the dead
  // object does.
  std::map<ObjectId, ObjectId> last_striker;
  std::uint64_t ended_at = 0;
  // `IMRUN_OVERLAPS=<n>`: the overlap census every n turns, and its worst.
  std::uint64_t overlap_every = 0;
  if (const char* every = std::getenv("IMRUN_OVERLAPS")) overlap_every = std::strtoull(every, nullptr, 10);
  sim::MovementSystem::OverlapCensus overlap_worst;
  std::uint64_t overlap_worst_turn = 0;
  // `IMRUN_PILES=1`: with the census, where the bodies are piled -- every
  // 16-unit cell that three or more standing bodies share (standing as the
  // census means it: a live unit on the map with no route), with each body's
  // class, owner and running order, and the hero whose army it is in.
  // A pair count says how bad; this says which order, which player and which
  // spot, which is how the piles at p1's door on Crossroads were found -- and
  // which army, which is how a hero's recruits stopped in one pile at the door
  // they left by were.
  const bool show_piles = std::getenv("IMRUN_PILES") != nullptr;
  const auto list_piles = [](sim::World& world, std::uint64_t at) {
    const ClassGraph* graph = world.class_graph();
    const sim::CommandSystem* commands = sim::command_system(world);
    std::map<std::pair<std::int32_t, std::int32_t>, std::vector<ObjectId>> cells;
    for (const sim::WorldObject& slot : world.objects()) {
      const sim::ObjectState& s = slot.state;
      if (!s.flags.is_unit || s.is_held() || s.flags.unspawned || s.flags.in_air) continue;
      if (s.health == 0 || s.flags.has_active_path || s.position == sim::kHeldPosition) continue;
      cells[{s.position.x / 16, s.position.y / 16}].push_back(slot.id);
    }
    for (const auto& [cell, ids] : cells) {
      if (ids.size() < 3) continue;
      std::printf("  pile turn %llu: (%d,%d) x%zu:", static_cast<unsigned long long>(at),
                  cell.first * 16, cell.second * 16, ids.size());
      for (const ObjectId id : ids) {
        const sim::WorldObject* slot = world.find(id);
        const bool named = graph != nullptr && slot->class_index != kNoClass;
        std::printf(" %u %s p%d %s", id,
                    named ? std::string(graph->at(slot->class_index).id).c_str() : "?",
                    static_cast<int>(slot->state.owner),
                    commands != nullptr && commands->command_count(id) > 0
                        ? std::string(commands->command_name(id, 0)).c_str()
                        : "-");
        const sim::HeroSystem* heroes = sim::hero_system_of(world);
        const ObjectId hero = heroes != nullptr ? heroes->hero_of(id) : kNoObject;
        if (hero != kNoObject) {
          std::printf(" h%u", hero);
        } else {
          std::printf(" h-");
        }
      }
      std::printf("\n");
    }
  };
  // With it, the melee census: every live melee unit with a live target no
  // more than `kFightReach` from it -- in the fight, not merely aimed at
  // something across the map -- is engaged (within its reach), closing (on a
  // route) or waiting (standing out of reach), summed over the sampled turns
  // and at the turn with the most.
  constexpr std::int64_t kFightReach = 400;
  struct MeleeCensus {
    std::size_t engaged = 0;
    std::size_t closing = 0;
    std::size_t waiting = 0;
    [[nodiscard]] std::size_t total() const noexcept { return engaged + closing + waiting; }
  };
  MeleeCensus melee_sum;
  MeleeCensus melee_biggest;
  std::uint64_t melee_biggest_turn = 0;
  const auto melee_census = [kFightReach](const sim::World& world, const sim::CombatSystem& fight) {
    MeleeCensus out;
    for (const sim::Combatant& c : fight.combatants()) {
      if (c.target == kNoObject || c.health <= 0 || c.action == sim::Action::dying) continue;
      const sim::ObjectState* self = world.state(c.id);
      if (self == nullptr || !self->flags.is_unit || self->is_held()) continue;
      if (fight.profile(c.class_index).is_ranged()) continue;
      const sim::Combatant* target = fight.find(c.target);
      const sim::ObjectState* them = world.state(c.target);
      if (target == nullptr || them == nullptr || them->health <= 0) continue;
      if (sim::dist_sq(self->position, them->position) > kFightReach * kFightReach) continue;
      sim::Combatant a = c;
      sim::Combatant d = *target;
      a.position = self->position;
      d.position = them->position;
      if (fight.in_attack_range(a, d)) {
        ++out.engaged;
      } else if (self->flags.has_active_path) {
        ++out.closing;
      } else {
        ++out.waiting;
      }
    }
    return out;
  };
  // `IMRUN_GOTO=<turn>:<id>:<x>,<y>`: before that turn, the object's owner
  // right-clicks the point with it selected -- the default order, as a click
  // gives it. `IMRUN_WATCH=<id>` prints where that object stands every ten
  // turns, and the gates its route crosses whenever they change. `IMRUN_GATES=1`
  // prints every gate that opens or closes and every one that starts or stops
  // letting units through, on the turn it happens, and every gate at the end.
  unsigned long long goto_turn = 0;
  unsigned goto_id = 0;
  int goto_x = 0;
  int goto_y = 0;
  bool goto_wanted = false;
  if (const char* order = std::getenv("IMRUN_GOTO")) {
    goto_wanted = std::sscanf(order, "%llu:%u:%d,%d", &goto_turn, &goto_id, &goto_x, &goto_y) == 4;
  }
  // `IMRUN_PLACE=<x>,<y>`: the ordered object is stood there first, so a test
  // can send a unit from where the map never puts one -- an enemy outside a
  // walled town.
  int place_x = 0;
  int place_y = 0;
  bool place_wanted = false;
  if (const char* place = std::getenv("IMRUN_PLACE")) {
    place_wanted = std::sscanf(place, "%d,%d", &place_x, &place_y) == 2;
  }
  ObjectId watched = kNoObject;
  if (const char* watch = std::getenv("IMRUN_WATCH")) watched = static_cast<ObjectId>(std::strtoul(watch, nullptr, 10));
  const bool show_gates = std::getenv("IMRUN_GATES") != nullptr;
  std::string watched_crossing;
  std::map<ObjectId, std::pair<bool, bool>> gate_seen;  // target open, lets through
  // `IMRUN_ECONOMY=<n>`: every n turns, one line per player-owned town and
  // village -- population, gold, food -- because a stronghold that never
  // reaches `ESH_BUILDARMY.VS`'s gold floor is a curve, not an end state.
  std::uint64_t economy_every = 0;
  if (const char* every = std::getenv("IMRUN_ECONOMY")) economy_every = std::strtoull(every, nullptr, 10);
  for (std::uint64_t turn = 0; turn < turns; ++turn) {
    if (until_over && ended_at != 0) break;
    if (economy_every != 0 && turn % economy_every == 0) {
      if (const sim::EconomySystem* economy = sim::economy_of(run.world())) {
        for (const sim::Settlement& set : economy->settlements().all()) {
          if (set.owner >= 14 || (set.kind != sim::SettlementKind::stronghold && set.kind != sim::SettlementKind::village)) continue;
          std::printf("  economy turn %llu #%u p%d kind %d pop %d/%d gold %d food %d loan %d\n",
                      static_cast<unsigned long long>(turn), set.id, static_cast<int>(set.owner),
                      static_cast<int>(set.kind), set.population, set.max_population,
                      set.warehouse.gold, set.warehouse.food, set.loan);
        }
      }
    }
    // A traced run marks its turns, so a burst of calls can be placed in time.
    if (trace.scheduler != nullptr && trace.printed < trace.limit) {
      std::printf("  -- turn %llu\n", static_cast<unsigned long long>(turn + 1));
    }
    if (goto_wanted && turn == goto_turn && place_wanted) {
      // Off any route it was walking first, or the route puts it back.
      if (sim::MovementSystem* move = sim::movement_system(run.world())) {
        move->stop(run.world(), goto_id);
      }
      const bool placed = run.world().set_position(goto_id, sim::Point{place_x, place_y});
      // And out of its AI's hands: its no-AI flag set, as a map can author it
      // (`UNITFLAG_NOAI`), and a squad of its own, which takes `SF_NOAI` from
      // that flag (0x00447330), so `SQUADMONITOR.VS` passes it by. What the
      // run then shows is the order and the world's answer to it, not its
      // AI's next thought -- a hurt squad flees home.
      if (sim::ObjectState* state = run.world().mutable_state(goto_id); placed && state != nullptr) {
        state->flags.no_ai = true;
        if (sim::HeroSystem* heroes = sim::hero_system_of(run.world())) {
          const ObjectId alone[] = {static_cast<ObjectId>(goto_id)};
          sim::regroup_into_fresh_squads(run.world(), *heroes, alone, /*SS_IDLE*/ 0,
                                         run.world().time());
        }
      }
      std::printf("  place turn %llu: %u at (%d,%d)%s\n", turn, goto_id, place_x, place_y,
                  placed ? "" : " refused");
    }
    if (goto_wanted && turn == goto_turn) {
      const sim::WorldObject* actor = run.world().find(goto_id);
      const ObjectId actors[] = {static_cast<ObjectId>(goto_id)};
      const sim::CommandTable* table = sim::order_command_table(run.world());
      sim::OrderReport made;
      if (actor != nullptr && table != nullptr) {
        sim::OrderTarget target;
        target.point = sim::Point{goto_x, goto_y};
        made = sim::issue_default_order(run.world(), *table, actors, target, sim::OrderMode::replace,
                                        false, actor->state.owner, nullptr);
      }
      std::printf("  goto turn %llu: %u to (%d,%d), %zu issued\n", turn, goto_id, goto_x, goto_y,
                  made.issued);
      // The order's route is printed whatever it crosses: a route that
      // crosses no gate after one that crossed none before is still the
      // answer to this order, and a reader waits for it.
      if (goto_id == watched) watched_crossing = "\x01";
    }
    run.advance(1, length);
    if (watched != kNoObject) {
      // The gates the watched unit's route crosses, whenever they change: a
      // route a gate bars is searched again round it (`sim/gate.hpp`).
      std::string crossing;
      if (const sim::MovementSystem* move = sim::movement_system(run.world())) {
        if (const sim::MoveState* state = move->find(watched); state != nullptr && state->has_path) {
          for (const sim::GateCrossing& c : state->gate_crossings) {
            crossing += (crossing.empty() ? "" : ",") + std::to_string(c.gate);
          }
        }
      }
      if (crossing != watched_crossing) {
        std::printf("  route turn %llu: %u crosses %s\n", static_cast<unsigned long long>(turn + 1),
                    watched, crossing.empty() ? "no gate" : crossing.c_str());
        watched_crossing = crossing;
      }
    }
    if (watched != kNoObject && (turn + 1) % 10 == 0) {
      const sim::Point at = run.world().resolve_position(watched);
      std::printf("  watch turn %llu: %u at (%d,%d)\n", static_cast<unsigned long long>(turn + 1),
                  watched, at.x, at.y);
    }
    if (show_gates) {
      for (const sim::WorldObject& slot : run.world().objects()) {
        if (slot.object == nullptr || !slot.object->is_a(NativeClass::gate)) continue;
        const std::pair<bool, bool> now{slot.state.flags.gate_open,
                                        sim::gate_lets_through(slot, run.world().time())};
        const auto seen = gate_seen.find(slot.id);
        if (seen != gate_seen.end() && seen->second == now) continue;
        if (seen != gate_seen.end() && seen->second.first != now.first) {
          std::printf("  gate turn %llu: %u %s\n", static_cast<unsigned long long>(turn + 1), slot.id,
                      now.first ? "opens" : "closes");
        }
        if (seen != gate_seen.end() && seen->second.second != now.second) {
          std::printf("  gate turn %llu: %u %s\n", static_cast<unsigned long long>(turn + 1), slot.id,
                      now.second ? "lets units through" : "stops letting units through");
        }
        gate_seen[slot.id] = now;
      }
    }
    if (ended_at == 0 && run.match_status().over) ended_at = turn + 1;
    if (overlap_every != 0 && (turn + 1) % overlap_every == 0) {
      if (const sim::MovementSystem* move = sim::movement_system(run.world())) {
        const auto census = move->overlap_census(run.world());
        if (show_piles) list_piles(run.world(), turn + 1);
        if (census.touching > overlap_worst.touching) {
          overlap_worst = census;
          overlap_worst_turn = turn + 1;
        }
      }
      if (const sim::CombatSystem* fight = sim::combat_system_of(run.world())) {
        const MeleeCensus melee = melee_census(run.world(), *fight);
        melee_sum.engaged += melee.engaged;
        melee_sum.closing += melee.closing;
        melee_sum.waiting += melee.waiting;
        if (melee.total() > melee_biggest.total()) {
          melee_biggest = melee;
          melee_biggest_turn = turn + 1;
        }
      }
    }
    const sim::CombatSystem* fight = sim::combat_system_of(run.world());
    if (fight == nullptr) continue;
    for (const sim::CombatEvent& event : fight->events()) {
      switch (event.kind) {
        // `strike` is every damage application, melee and arrow alike:
        // `CombatSystem::land` resolves an impact through the same `hit` that a
        // melee blow goes through, and `hit` is what emits this. **Only this
        // one carries the resolved figure** -- an `impact` event carries the
        // shot's raw `attack` before armour, so summing both would double-count
        // every arrow and overstate the total by a factor of nearly three.
        case sim::CombatEvent::Kind::strike:
          ++strikes;
          dealt += event.damage;
          if (show_deaths) last_striker[event.defender] = event.attacker;
          break;
        case sim::CombatEvent::Kind::launch: ++launches; break;
        case sim::CombatEvent::Kind::impact: ++impacts; break;
        case sim::CombatEvent::Kind::death:
          ++deaths;
          // `IMRUN_DEATHS=1`: every death, where and to whom -- the turn, the
          // dead object's class, owner and position, and its last striker's. The
          // question "where did this player's losses happen" is one a total
          // cannot answer.
          if (show_deaths) {
            const ClassGraph* graph = run.world().class_graph();
            const auto name_of = [&](ObjectId id) -> std::string {
              const sim::WorldObject* slot = run.world().find(id);
              if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return "?";
              return std::string(graph->at(slot->class_index).id);
            };
            const auto owner_of = [&](ObjectId id) -> int {
              const sim::ObjectState* state = run.world().state(id);
              return state == nullptr ? -1 : static_cast<int>(state->owner);
            };
            const auto at = run.world().resolve_position(event.defender);
            ObjectId by = event.attacker;
            if (by == kNoObject) {
              const auto hit = last_striker.find(event.defender);
              if (hit != last_striker.end()) by = hit->second;
            }
            std::printf("  death turn %llu: %u %s p%d at (%d,%d) by %u %s p%d\n",
                        static_cast<unsigned long long>(turn + 1), event.defender,
                        name_of(event.defender).c_str(), owner_of(event.defender), at.x, at.y,
                        by, name_of(by).c_str(), owner_of(by));
          }
          break;
      }
    }
  }
  const sim::SessionReport report = run.report();

  std::printf("after %llu turns of %dms:\n", static_cast<unsigned long long>(report.turns),
              length);
  std::printf("  running   %zu\n", report.scripts_running);
  // Where the running scripts are, by file: the AI tree is 65 files that sleep
  // and poll, and "1,142 running" says nothing about whether an army is being
  // raised or every recruiter is parked on its first `Sleep`. Top ten, by
  // count, ties by name.
  {
    std::map<std::string, std::size_t> by_file;
    const script::Scheduler& scheduler = run.scheduler();
    for (const script::ScriptRecord& record : scheduler.scripts()) {
      if (record.execution.status == script::ExecStatus::finished ||
          record.execution.status == script::ExecStatus::failed) {
        continue;
      }
      if (record.chunk_index >= scheduler.chunk_count()) continue;
      ++by_file[scheduler.chunk(record.chunk_index).source_name];
    }
    std::vector<std::pair<std::string, std::size_t>> rows(by_file.begin(), by_file.end());
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    std::size_t shown = 0;
    for (const auto& [file, count] : rows) {
      if (shown++ == 10) break;
      std::printf("            %5zu  %s\n", count, file.c_str());
    }
    // The orders standing at the end, by verb, over every object -- what the
    // scripts above have actually told the world to do -- and the verbs that
    // could not be launched at all.
    if (const sim::CommandSystem* commands = sim::command_system(run.world())) {
      std::map<std::string, std::size_t> by_verb;
      for (const sim::WorldObject& slot : run.world().objects()) {
        const std::size_t n = commands->command_count(slot.id);
        for (std::size_t i = 0; i < n; ++i) {
          by_verb[std::string(commands->command_name(slot.id, i))] += 1;
        }
      }
      std::vector<std::pair<std::string, std::size_t>> verbs(by_verb.begin(), by_verb.end());
      std::stable_sort(verbs.begin(), verbs.end(),
                       [](const auto& a, const auto& b) { return a.second > b.second; });
      std::printf("  orders\n");
      std::size_t listed = 0;
      for (const auto& [verb, count] : verbs) {
        if (listed++ == 12) break;
        std::printf("            %5zu  %s\n", count, verb.c_str());
      }
      for (const auto& [verb, count] : commands->launch_failures()) {
        std::printf("            %5zu  %s  (could not be launched)\n", count, verb.c_str());
      }
    }
    // `IMRUN_SETTLEMENTS=1`: every settlement, with what the AI's economy
    // scripts ask of it -- owner, kind, gold and food, the garrison, and the
    // buildings by class -- because "no army was raised" is a sentence about
    // a settlement before it is one about a script.
    if (std::getenv("IMRUN_SETTLEMENTS") != nullptr) {
      if (const sim::EconomySystem* economy = sim::economy_of(run.world())) {
        std::printf("  settlements\n");
        const ClassGraph* graph = run.world().class_graph();
        for (const sim::Settlement& set : economy->settlements().all()) {
          // By the world's back-link rather than `Settlement::buildings`, which
          // holds the anchor alone on a loaded map; the walls, towers and
          // barracks are what the question is about.
          std::map<std::string, std::size_t> classes;
          for (const sim::WorldObject& slot : run.world().objects()) {
            if (slot.internal != sim::InternalKind::none || slot.settlement != set.object) continue;
            if (!slot.state.flags.is_building) continue;
            if (graph == nullptr || slot.class_index == kNoClass) continue;
            classes[std::string(graph->at(slot.class_index).id)] += 1;
          }
          std::printf("            #%u p%d kind %d pop %d/%d gold %d food %d garrison %d/%d sentries %d/%d"
                      " loyalty %d:",
                      set.id, static_cast<int>(set.owner), static_cast<int>(set.kind), set.population,
                      set.max_population,
                      set.warehouse.gold, set.warehouse.food, set.holder.count(),
                      set.holder.max_units, set.sentries, set.max_sentries, set.loyalty);
          for (const auto& [name, count] : classes) std::printf(" %s x%zu", name.c_str(), count);
          std::printf("\n");
        }
      }
    }
    // `IMRUN_OBJECTS=<text>[,<text>...]`: every object whose class id
    // contains any of them, with its owner, position, holder, health and
    // running order -- where a settlement's sentries are, beside its walls and
    // gates, is a question the settlement's counts cannot answer.
    if (const char* needle = std::getenv("IMRUN_OBJECTS")) {
      std::vector<std::string> needles;
      for (std::string_view rest = needle; !rest.empty();) {
        const std::size_t comma = rest.find(',');
        if (comma != 0) needles.emplace_back(rest.substr(0, comma));
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
      }
      const ClassGraph* graph = run.world().class_graph();
      const sim::CommandSystem* commands = sim::command_system(run.world());
      std::printf("  objects matching %s\n", needle);
      for (const sim::WorldObject& slot : run.world().objects()) {
        if (slot.internal != sim::InternalKind::none) continue;
        if (graph == nullptr || slot.class_index == kNoClass) continue;
        const std::string id(graph->at(slot.class_index).id);
        bool wanted = false;
        for (const std::string& n : needles) wanted = wanted || id.find(n) != std::string::npos;
        if (!wanted) continue;
        std::printf("            %u %s p%d at (%d,%d) holder %u health %d%s%s order %s\n", slot.id,
                    id.c_str(), static_cast<int>(slot.state.owner), slot.state.position.x,
                    slot.state.position.y, slot.state.holder, slot.state.health,
                    slot.state.flags.unspawned ? " unspawned" : "",
                    slot.state.flags.hidden ? " hidden" : "",
                    commands != nullptr && commands->command_count(slot.id) > 0
                        ? std::string(commands->command_name(slot.id, 0)).c_str()
                        : "-");
      }
    }
    if (show_gates) {
      std::printf("  gates     %zu line(s)\n", run.world().gate_lines().lines().size());
      for (const sim::WorldObject& slot : run.world().objects()) {
        if (slot.object == nullptr || !slot.object->is_a(NativeClass::gate)) continue;
        const sim::GateLines::Line* line = run.world().gate_lines().find(slot.id);
        std::printf("            gate %u p%d at (%d,%d) target %s, %s, %zu line cell(s)%s%s\n",
                    slot.id, static_cast<int>(slot.state.owner), slot.state.position.x,
                    slot.state.position.y, slot.state.flags.gate_open ? "open" : "closed",
                    sim::gate_lets_through(slot, run.world().time()) ? "lets units through"
                                                                     : "barred",
                    line == nullptr ? 0 : line->cells.size(),
                    slot.state.flags.enemies_near ? ", enemies near" : "",
                    slot.state.flags.friends_near ? ", friends near" : "");
      }
    }
    // And the AI tree on its own, every file, because that is the question
    // this histogram was added to answer.
    std::printf("  ai scripts\n");
    for (const auto& [file, count] : rows) {
      if (file.rfind("DATA/AI/", 0) != 0 && file.rfind("data/ai/", 0) != 0) continue;
      std::printf("            %5zu  %s\n", count, file.c_str());
    }
  }
  // The mission's objectives, *after* the turns -- the pre-run count is always
  // zero, because a note is given by a script and no script has run yet. That
  // is the whole reason this line is down here and the declared count is up
  // there: the two answer different questions and putting them together made
  // the second one read as a broken pipe for an afternoon.
  if (const sim::CampaignSystem* campaign = sim::campaign_system_of(run.world())) {
    std::printf("  notes     %zu given of %zu declared\n", campaign->note_board().size(),
                campaign->notes().size());
  }
  std::printf("  finished  %zu\n", report.scripts_finished);
  // Whether units kept out of each other's way, and how. A cooperative step
  // that never fired on a real map would look exactly like one that works.
  if (const sim::MovementSystem* move = sim::movement_system(run.world())) {
    const sim::MovementSystem::AvoidanceCounters& a = move->avoidance();
    std::printf("  avoided   %llu steps tested, %llu taken: %llu holds, %llu early, %llu "
                "sidesteps; %zu ownerless locks\n",
                static_cast<unsigned long long>(a.decisions),
                static_cast<unsigned long long>(a.blocked),
                static_cast<unsigned long long>(a.holds),
                static_cast<unsigned long long>(a.early),
                static_cast<unsigned long long>(a.sidesteps), move->static_locks().size());
    std::printf("  gates     %llu route(s) searched again round a gate, %llu wait(s) before one\n",
                static_cast<unsigned long long>(a.gate_searches),
                static_cast<unsigned long long>(a.gate_waits));
    std::printf("  free spot %llu full band(s) searched out from, %llu route(s) re-aimed\n",
                static_cast<unsigned long long>(a.free_spot_searches),
                static_cast<unsigned long long>(a.free_spot_aims));
    // Bodies drawn through each other (playtest report #13), as the run ends
    // and, sampled, at its worst.
    const sim::MovementSystem::OverlapCensus census = move->overlap_census(run.world());
    std::printf("  overlaps  %zu standing bod(ies): %zu pair(s) nearer than their radii, %zu stacked\n",
                census.bodies, census.touching, census.stacked);
    if (overlap_every != 0) {
      std::printf("  overlaps  worst at turn %llu: %zu pair(s) of %zu bod(ies), %zu stacked\n",
                  static_cast<unsigned long long>(overlap_worst_turn), overlap_worst.touching,
                  overlap_worst.bodies, overlap_worst.stacked);
      std::printf("  melee     sampled: %zu engaged, %zu closing, %zu waiting out of reach\n",
                  melee_sum.engaged, melee_sum.closing, melee_sum.waiting);
      std::printf("  melee     biggest at turn %llu: %zu engaged, %zu closing, %zu waiting\n",
                  static_cast<unsigned long long>(melee_biggest_turn), melee_biggest.engaged,
                  melee_biggest.closing, melee_biggest.waiting);
    }
  }
  if (const sim::CombatSystem* fight = sim::combat_system_of(run.world())) {
    std::size_t dying = 0;
    std::int64_t health_after = 0;
    for (const sim::Combatant& unit : fight->combatants()) {
      health_after += unit.health;
      if (!unit.alive || unit.action == sim::Action::dying) ++dying;
    }
    const std::size_t combat_after = fight->combatants().size();
    std::printf("  combat    %zu enrolled (was %zu), %zu dying, %zu shot(s) in flight\n",
                combat_after, combat_before, dying, fight->projectiles().size());
    std::printf("  strikes   %zu blow(s) landed, of which %zu arrived as one of %zu shot(s)\n",
                strikes, impacts, launches);
    std::printf("  kills     %zu death event(s), %zu combatant(s) gone from the roll\n", deaths,
                combat_before > combat_after ? combat_before - combat_after : 0);
    // Net, not "lost": the roll grows during a run. `SpawnGroup` brings a
    // mission's reinforcements in mid-match, and each one enrols with a full
    // health bar, so the total can end higher than it started -- Zama's ends
    // 30,000 up. `dealt` is the damage figure; this pair is the roll's size.
    std::printf("  health    %lld now against %lld at turn 0 (%+lld net); %lld damage dealt\n",
                static_cast<long long>(health_after), static_cast<long long>(health_before),
                static_cast<long long>(health_after - health_before),
                static_cast<long long>(dealt));
  }
  std::printf("  hash      %016llx\n\n",
              static_cast<unsigned long long>(report.hash));

  // Whether the match ended, and what said so. `EndGame` is the only thing that
  // can move this, so a non-`undecided` outcome here is proof that a shipped
  // victory script or mission sequence reached its ending -- which is a much
  // stronger statement than "the entry point is registered".
  {
    const sim::MatchStatus status = run.match_status();
    std::printf("match     over=%s winner=%d human=%d outcome=%d%s%.*s\n",
                status.over ? "yes" : "no", static_cast<int>(status.winner),
                static_cast<int>(status.human), static_cast<int>(status.human_outcome),
                status.message.empty() ? "" : " message=",
                static_cast<int>(status.message.size()), status.message.data());
    if (ended_at != 0) {
      std::printf("          ended at turn %llu\n", static_cast<unsigned long long>(ended_at));
    }
    // Per-slot, because the summary can only say "not over" while individual
    // players have already been decided -- and a decided player is an `EndGame`
    // that actually ran.
    if (const sim::MatchSystem* match = sim::match_system_of(run.world())) {
      std::size_t decided = 0;
      for (std::size_t i = 0; i < sim::kPlayerCount; ++i) {
        const auto id = static_cast<sim::PlayerId>(i);
        if (!match->player(id).participates) continue;
        if (match->player(id).outcome == sim::MatchOutcome::undecided) continue;
        if (decided == 0) std::printf("          decided by EndGame:");
        ++decided;
        std::printf(" p%zu=%s", i,
                    match->player(id).outcome == sim::MatchOutcome::won ? "won" : "lost");
      }
      if (decided != 0) std::printf("  (%zu of %zu)\n", decided, sim::kPlayerCount);
      // The end-of-match report's counters, per participant: what the
      // Statistics screen would show, and the cheapest proof that the
      // world tells the match about its soldiers and its gold.
      for (std::size_t i = 0; i < sim::kPlayerCount; ++i) {
        const auto id = static_cast<sim::PlayerId>(i);
        if (!match->player(id).participates) continue;
        const sim::PlayerScoreCounters& c = match->score(id);
        if (c.units_produced == 0 && c.units_lost == 0 && c.gold == 0 && c.gold_townhall == 0) continue;
        // Beside it, the soldiers standing now, so the two can be compared.
        std::size_t standing = 0;
        const sim::ClassFilter military = sim::ClassFilter::parse("Military", run.world().class_graph());
        for (const sim::WorldObject& slot : run.world().objects()) {
          if (slot.state.owner != id || slot.state.flags.unspawned || !slot.state.flags.is_unit) continue;
          if (run.world().matches_filter(slot, military)) ++standing;
        }
        std::printf("          report p%zu: units %d produced %d killed %d lost %d most (%zu standing); gold %d spent %d taxes %d other %d captured; food %d spent\n",
                    i, c.units_produced, c.units_killed, c.units_lost, c.units_max, standing, c.gold, c.gold_townhall,
                    c.gold_outpost, c.gold_captured, c.food);
      }
    }
  }

  // The root scope of the environment store, which is where a container's own
  // sequences keep their mission state -- and, in the conquest, where
  // `ConquestBonus` lands. Printing it is the cheapest proof that the campaign
  // path ran at all rather than merely being registered.
  if (const sim::EnvSystem* env = sim::env_of(run.world())) {
    std::size_t roots = 0;
    for (const sim::EnvEntry& entry : env->env().entries()) {
      if (entry.scope.kind != sim::EnvScopeKind::root) continue;
      if (roots == 0) std::printf("environment, root scope:\n");
      ++roots;
      std::printf("  %-32s = \"%s\"\n", entry.key.c_str(), entry.value.c_str());
    }
    if (roots != 0) std::printf("\n");
  }

  // What the mission leaves for the next one. Written only on a won match
  // -- a lost one leaves the file as it was, which is the shipped rule -- and
  // `--declare-won` is the knob that makes the path reachable headless.
  if (!campaign_path.empty()) {
    if (declare_won && human != kNoPlayer) {
      run.declare_match(human, /*lost=*/false);
      std::printf("declared  the human's match won by --declare-won, not by any script\n");
    }
    const sim::MatchStatus status = run.match_status();
    if (status.human_won) {
      sim::MissionResult result;
      result.map_number = std::atoi(imperivm::gamedata::map_number(payloads.map_directory).c_str());
      result.won = true;
      if (!inputs.conquest.empty()) {
        const auto conquest = sim::ConquestMap::parse(inputs.conquest);
        if (conquest.ok()) {
          result.territory = imperivm::gamedata::territory_of_map(container, *conquest,
                                                                  payloads.map_directory);
        }
      }
      const sim::CampaignCarry carry = run.campaign_carry(container_relative, result);
      std::string why;
      if (!imperivm::gamedata::write_campaign_file(campaign_path, carry, &why)) {
        std::fprintf(stderr, "%s\n", why.c_str());
        return 1;
      }
      std::printf("carry     wrote %s: territory %d, %zu conquered, bonus \"%s\"\n",
                  campaign_path.c_str(), result.territory, carry.progress.conquered.size(),
                  carry.progress.active_bonus.c_str());
    }
  }

  if (!report.launch_failures.empty()) {
    std::printf("%zu verb(s) whose script would not compile:\n", report.launch_failures.size());
    for (const sim::SessionReport::LaunchFailure& miss : report.launch_failures) {
      std::printf("  %5zu  %s\n", miss.commands, miss.verb.c_str());
    }
  }
  if (report.traps.empty()) {
    std::printf("no traps.\n");
    return 0;
  }
  std::printf("%zu distinct traps, most-hit first:\n", report.traps.size());
  for (const sim::SessionReport::Trap& trap : report.traps) {
    std::printf("  %5zu  %s\n", trap.scripts, trap.message.c_str());
  }
  return 0;
}
