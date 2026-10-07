// The determinism harness's front end: run the conformance checks and print
// what they found.
//
// The harness itself is `engine/core/src/sim/conformance.cpp` and knows nothing
// about files, because the core may not. This is the half that opens a game
// installation, builds a session from a shipped map, writes a golden trace to
// disk and prints a report. Same split as `imcheck` and `imrun`, for the same
// reason.
//
// Three subcommands, in increasing order of how much they prove:
//
//   self    the built-in synthetic scenario. Needs no game data, so CI runs it
//           on a machine that has never seen the game. Proves the harness works.
//   trace   print that scenario's trace, which is how the golden literal in
//           `engine/tests/test_conformance.cpp` was produced.
//   map     a real `GameSession` over a shipped map. Proves the *simulation* is
//           deterministic, which is the thing anybody actually cares about.
//
// `map` takes an optional golden file. With `--record` it writes one; without,
// it compares against it and fails on any difference. That is the mechanism by
// which a change that alters simulation results has to say so out loud: the
// golden file moves in the diff, and somebody has to explain why.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>

#include <algorithm>
#include <cmath>
#include <array>
#include <map>
#include <set>

#include "imperivm/core/formats/bfhp.hpp"
#include "imperivm/core/formats/lzis.hpp"
#include "imperivm/core/formats/pak.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/anim.hpp"
#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netjoin.hpp"
#include "imperivm/core/sim/netlobby.hpp"
#if IMPERIVM_HAVE_NET
#include "imperivm/net/match.hpp"
#include "imperivm/net/udp.hpp"
#endif
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/cmdbar.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/infobar.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/gamedata/installation.hpp"
#include "imperivm/gamedata/map_source.hpp"

using namespace imperivm::core;
namespace conformance = imperivm::core::sim::conformance;

namespace {

std::vector<std::byte> read_file(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return {};
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::byte> data(static_cast<std::size_t>(size < 0 ? 0 : size));
  if (!data.empty() && std::fread(data.data(), 1, data.size(), file) != data.size()) {
    data.clear();
  }
  std::fclose(file);
  return data;
}

bool write_text(const std::string& path, std::string_view text) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), file) == text.size();
  std::fclose(file);
  return ok;
}

std::string upper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == '/') c = '\\';
  }
  return out;
}

// -- loading a real installation -------------------------------------------
//
// Deliberately a copy of what `imrun.cpp` does rather than a shared header.
// Two tools that both happen to open a game directory is not yet a library,
// and the alternative -- editing a file another agent owns to extract one --
// costs more than the duplication does.

/// Serves `.vs` source out of an already-open pack, case- and
/// separator-insensitively, falling back to the basename.
class PackScripts final : public imperivm::core::sim::ScriptResolver {
 public:
  explicit PackScripts(const PakDirectory& pack) : pack_(&pack) {
    for (const PakEntry& entry : pack.entries()) {
      const std::string name = upper(entry.name);
      if (!name.ends_with(".VS")) continue;
      by_path_.emplace_back(name, &entry);
      const std::size_t slash = name.find_last_of('\\');
      by_base_.emplace_back(slash == std::string::npos ? name : name.substr(slash + 1), &entry);
    }
  }

  std::span<const std::byte> source(std::string_view path) override {
    const std::string wanted = upper(path);
    const PakEntry* entry = find(by_path_, wanted);
    if (entry == nullptr) {
      const std::size_t slash = wanted.find_last_of('\\');
      entry = find(by_base_, slash == std::string::npos ? wanted : wanted.substr(slash + 1));
    }
    if (entry == nullptr) return {};
    const auto blob = pack_->read(*entry);
    return blob.ok() ? blob.value() : std::span<const std::byte>{};
  }

 private:
  using Table = std::vector<std::pair<std::string, const PakEntry*>>;
  static const PakEntry* find(const Table& table, const std::string& key) {
    for (const auto& [name, entry] : table) {
      if (name == key) return entry;
    }
    return nullptr;
  }
  const PakDirectory* pack_;
  Table by_path_;
  Table by_base_;
};

/// Everything read out of a map `.bfhp`, kept alive for the scenario's lifetime.
struct Container {
  std::vector<std::byte> bytes;
  std::vector<std::byte> map_objects;
  std::vector<std::vector<std::byte>> players;
  std::vector<std::span<const std::byte>> player_spans;
};

bool load_container(const std::string& path, Container& out) {
  out.bytes = read_file(path);
  if (out.bytes.empty()) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    return false;
  }
  // Some shipped containers are an LZIS stream wrapping the container.
  auto file = BlockFile::open(out.bytes);
  if (!file.ok()) {
    const auto header = parse_lzis_header(out.bytes);
    if (header.ok()) {
      std::vector<std::byte> plain(header.value().uncompressed_size);
      if (lzis_decompress(out.bytes, plain).ok()) {
        out.bytes = std::move(plain);
        file = BlockFile::open(out.bytes);
      }
    }
  }
  if (!file.ok()) {
    std::fprintf(stderr, "%s is not an HPFS container\n", path.c_str());
    return false;
  }
  const auto index = BlockFileIndex::build(file.value());
  if (!index.ok()) return false;

  out.players.resize(imperivm::core::sim::kPlayerCount);
  for (const auto& entry : index.value().entries()) {
    if (entry.is_dir) continue;
    std::vector<std::byte> payload(entry.size);
    if (!index.value().read(entry, payload).ok()) continue;

    const std::string name = upper(entry.path);
    if (name.ends_with("MAP.OBJ.XML") && out.map_objects.empty()) {
      out.map_objects = std::move(payload);
    } else if (name.starts_with("PLAYER") && name.ends_with(".XML")) {
      const std::string digits = name.substr(6, name.size() - 10);
      const int id = std::atoi(digits.c_str());
      if (id >= 0 && id < static_cast<int>(imperivm::core::sim::kPlayerCount) &&
          !digits.empty()) {
        out.players[static_cast<std::size_t>(id)] = std::move(payload);
      }
    }
  }
  for (const auto& document : out.players) out.player_spans.push_back(document);
  return !out.map_objects.empty();
}

/// Everything a map scenario needs, loaded once and shared by every run.
///
/// The class graph, the host registry and the script pack are world-invariant
/// -- `session.hpp` says so of the registry and the same argument holds for the
/// other two -- so loading them per run would only make the harness slower and
/// give the scenario a chance to differ between instantiations.
struct Installation {
  std::vector<std::byte> pack_bytes;
  std::vector<std::vector<std::byte>> class_blobs;
  ClassGraph graph;
  std::vector<std::byte> constants;
  std::vector<std::byte> ai_profile;
  Container container;
  script::HostRegistry registry;
  std::size_t implemented = 0;
  std::size_t class_files = 0;
  std::unique_ptr<PakDirectory> pack;
  std::unique_ptr<PackScripts> scripts;
  /// `DATA/COMMANDS/*.XML`, in sorted name order: what a right click
  /// resolves through. Loaded because the lockstep check issues orders, and
  /// a session without the table answers every one of them "nothing to do".
  std::vector<std::vector<std::byte>> command_blobs;
  std::vector<std::span<const std::byte>> command_spans;
  /// What the info bar reads, for `observe`, which drives it the way a
  /// screen does.
  std::vector<std::byte> skills;
  std::vector<std::byte> unit_specials;
};

bool load_installation(const std::string& game, const std::string& map, Installation& out) {
  out.pack_bytes = read_file(game + "/Packs/data.pak");
  if (out.pack_bytes.empty()) {
    std::fprintf(stderr, "cannot read %s/Packs/data.pak\n", game.c_str());
    return false;
  }
  auto directory = PakDirectory::parse(out.pack_bytes);
  if (!directory.ok()) {
    std::fprintf(stderr, "data.pak did not parse\n");
    return false;
  }
  out.pack = std::make_unique<PakDirectory>(std::move(directory.value()));

  for (const PakEntry& entry : out.pack->entries()) {
    const std::string name = upper(entry.name);
    if (!name.starts_with("DATA\\CLASSES\\") || !name.ends_with(".SC.XML")) continue;
    const auto blob = out.pack->read(entry);
    if (!blob.ok()) continue;
    out.class_blobs.emplace_back(blob.value().begin(), blob.value().end());
    if (out.graph.add(out.class_blobs.back(), name).ok()) ++out.class_files;
  }
  out.graph.link();

  const auto section = [&](const char* name) -> std::vector<std::byte> {
    const auto blob = out.pack->read(name);
    if (!blob.ok()) return {};
    return std::vector<std::byte>(blob.value().begin(), blob.value().end());
  };
  out.constants = section("DATA\\CONST.INI");
  out.ai_profile = section("DATA\\AI\\AI.INI");
  out.skills = section("DATA\\SKILLS.INI");
  out.unit_specials = section("DATA\\UNIT_SPECIALS.INI");

  if (!load_container(map, out.container)) return false;

  std::vector<std::string> command_names;
  for (const PakEntry& entry : out.pack->entries()) {
    const std::string name = upper(entry.name);
    if (!name.starts_with("DATA\\COMMANDS\\") || !name.ends_with(".XML")) continue;
    command_names.emplace_back(entry.name);
  }
  // Sorted, because merge order decides which of two rows with one name wins.
  std::sort(command_names.begin(), command_names.end());
  for (const std::string& name : command_names) {
    const auto blob = out.pack->read(name.c_str());
    if (!blob.ok()) continue;
    out.command_blobs.emplace_back(blob.value().begin(), blob.value().end());
  }
  for (const std::vector<std::byte>& blob : out.command_blobs) out.command_spans.emplace_back(blob);

  out.implemented = imperivm::core::sim::register_all_hosts(out.registry);
  out.scripts = std::make_unique<PackScripts>(*out.pack);
  return true;
}

/// A skirmish, loaded the way `imrun` and the app load one.
///
/// `imconform`'s own loader above builds a thin session: no terrain, no
/// passability, no entities, no settlement templates, and no `start_match`.
/// That is enough for an adventure map's standing armies to take orders, and
/// it is not a skirmish: the `Mutable` strongholds stay one building, nobody
/// trains, and no army ever marches. `--skirmish` builds the session from
/// `imperivm_gamedata` instead -- every input `imrun` gives it, the
/// container's own scripts first -- and starts it as a networked match.
struct Skirmish {
  imperivm::gamedata::Installation install;
  imperivm::gamedata::MapContainer container;
  imperivm::gamedata::MapPayloads payloads;
  std::unique_ptr<imperivm::gamedata::ContainerScripts> scripts;
  /// The setup. `false`, the default, is the app's networked match from the
  /// command line: every seat in `seats` is a human that gives no orders, the
  /// match's own human the lowest of them, every other seat the computer's.
  /// `true` is the map's own single-player setup exactly as `imrun` plays it
  /// -- the start player human and idle, everyone else the computer, the
  /// peers merely the seats that send the (empty) packets -- so that a
  /// networked run can be held against `imrun`'s number for the same map.
  bool map_setup = false;
  std::vector<imperivm::core::PlayerId> seats;
  /// The settings screen's starting gold (`MatchOptions::starting_gold`):
  /// -1 is "Default", the map's own; otherwise every settlement with a
  /// warehouse starts with it. A lobby rule the start carries, in either
  /// setup.
  std::int32_t gold = -1;
};

bool load_skirmish(const std::string& game, const std::string& map, Skirmish& out) {
  std::string error;
  if (!out.install.open(game, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  if (!out.container.open(map, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }
  out.payloads = imperivm::gamedata::read_payloads(out.container);
  if (!out.payloads.ok()) {
    std::fprintf(stderr, "no map.obj.xml in %s\n", map.c_str());
    return false;
  }
  out.scripts = std::make_unique<imperivm::gamedata::ContainerScripts>(out.container,
                                                                        out.install.scripts());
  return true;
}

/// What the AI's war did on one peer, turn by turn: the evidence that a
/// skirmish check checked a war and not an idle map, which the orders are on
/// an adventure map. Everything here is read from hashed state, so every peer
/// that agrees on the hashes tallies the same; it is printed, not compared.
struct WarTally {
  /// The turn count before this run's first turn: zero, or a late joiner's.
  std::uint64_t first_turn = 0;
  std::uint64_t last_turn = 0;
  /// Blows and deaths between two players' objects -- the map's own neutrals
  /// (the wildlife and the passive scenery) left out, so a sentry killing a
  /// wolf is not a war.
  std::size_t blows = 0;
  std::size_t kills = 0;
  std::uint64_t first_blow = 0;
  std::uint64_t first_kill = 0;
  /// Who struck the first blow and whom: `p2 ESentry1 5090 on p1 EHouse2
  /// 1456`. What tells a contact the map placed from a war the AI brought on.
  std::string first_blow_by;
  std::size_t captures = 0;
  std::size_t strongholds_taken = 0;
  std::uint64_t first_stronghold_taken = 0;
  /// Captures, defeats and the computer taking or leaving a seat, as they
  /// happened, in order.
  std::vector<std::string> events;
  std::uint64_t hash = 0;  ///< `GameSession::report().hash` at the end
  /// Whose screen the run was, as the session holds it: what proves each
  /// peer really was started from its own seat.
  imperivm::core::PlayerId screen = imperivm::core::kNoPlayer;
  /// The seats the computer played when the run began.
  std::vector<imperivm::core::PlayerId> computer_seats;

  // What the last turn left, to see what this one changed.
  std::map<imperivm::core::sim::ObjectId, imperivm::core::PlayerId> last_hit_by;
  std::vector<imperivm::core::PlayerId> owners;
  std::array<imperivm::core::sim::MatchOutcome, imperivm::core::sim::kPlayerCount> outcomes{};
  std::array<bool, imperivm::core::sim::kPlayerCount> computer{};

  void baseline(imperivm::core::sim::GameSession& session, std::uint64_t turn) {
    first_turn = last_turn = turn;
    screen = session.host_context().local_player;
    look(session, turn, /*say=*/false);
    for (std::size_t i = 0; i < computer.size(); ++i) {
      if (computer[i]) computer_seats.push_back(static_cast<imperivm::core::PlayerId>(i));
    }
  }

  void observe(imperivm::core::sim::GameSession& session, std::uint64_t turn) {
    using namespace imperivm::core::sim;
    last_turn = turn;
    World& world = session.world();
    const auto player_of = [&world](ObjectId id) {
      const WorldObject* object = world.find(id);
      return object == nullptr ? imperivm::core::kNoPlayer : object->state.owner;
    };
    const auto at_war = [](imperivm::core::PlayerId a, imperivm::core::PlayerId b) {
      return a < kNeutralWildlife && b < kNeutralWildlife && a != b;
    };
    if (const CombatSystem* combat = combat_system_of(world); combat != nullptr) {
      for (const CombatEvent& event : combat->events()) {
        if (event.kind == CombatEvent::Kind::strike) {
          const imperivm::core::PlayerId by = player_of(event.attacker);
          last_hit_by[event.defender] = by;
          if (at_war(by, player_of(event.defender)) && blows++ == 0) {
            first_blow = turn;
            const auto who = [&world](ObjectId id) {
              const WorldObject* object = world.find(id);
              if (object == nullptr) return "gone " + std::to_string(id);
              const imperivm::core::ClassGraph* graph = world.class_graph();
              std::string out = "p" + std::to_string(object->state.owner) + " ";
              if (graph != nullptr && object->class_index != imperivm::core::kNoClass) {
                out += std::string(graph->at(object->class_index).id) + " ";
              }
              return out + std::to_string(id);
            };
            first_blow_by = who(event.attacker) + " on " + who(event.defender);
          }
        } else if (event.kind == CombatEvent::Kind::death) {
          // A death names no killer: the last blow the dead took does.
          const auto hit = last_hit_by.find(event.defender);
          if (hit == last_hit_by.end()) continue;
          if (at_war(hit->second, player_of(event.defender)) && kills++ == 0) first_kill = turn;
          last_hit_by.erase(hit);
        }
      }
    }
    look(session, turn, /*say=*/true);
  }

 private:
  void look(imperivm::core::sim::GameSession& session, std::uint64_t turn, bool say) {
    using namespace imperivm::core::sim;
    World& world = session.world();
    const auto note = [&](std::string text) {
      if (say) events.push_back("turn " + std::to_string(turn) + ": " + std::move(text));
    };
    if (EconomySystem* economy = economy_of(world); economy != nullptr) {
      const std::span<const Settlement> all = economy->settlements().all();
      for (std::size_t i = 0; i < all.size(); ++i) {
        const imperivm::core::PlayerId owner = all[i].owner;
        if (i >= owners.size()) {
          owners.push_back(owner);
          continue;
        }
        if (owners[i] == owner) continue;
        const bool stronghold = all[i].kind == SettlementKind::stronghold;
        if (say) {
          ++captures;
          if (stronghold && strongholds_taken++ == 0) first_stronghold_taken = turn;
        }
        note("p" + std::to_string(owner) + " takes p" + std::to_string(owners[i]) + "'s " +
             (stronghold ? "stronghold" : "settlement") + " " + std::to_string(all[i].object));
        owners[i] = owner;
      }
    }
    const MatchSystem* match = match_system_of(world);
    const AiSystem* ai = ai_system_of(world);
    for (std::size_t i = 0; i < kPlayerCount; ++i) {
      const auto id = static_cast<imperivm::core::PlayerId>(i);
      if (match != nullptr && match->outcome(id) != outcomes[i]) {
        outcomes[i] = match->outcome(id);
        note("p" + std::to_string(i) +
             (outcomes[i] == MatchOutcome::lost ? " has lost" : " has won"));
      }
      const AiPlayer* seat = ai == nullptr ? nullptr : ai->player_ai(id);
      const bool on = seat != nullptr && seat->active;
      if (on != computer[i]) {
        computer[i] = on;
        note(std::string("the computer ") + (on ? "takes" : "leaves") + " p" + std::to_string(i));
      }
    }
  }
};

/// A peer's run over a `GameSession`: `SessionRun<>`'s two calls, and in a
/// skirmish the war's tally, handed to the scenario's log when the run ends
/// -- which is how a tally outlives a run `check_netplay` owns and destroys.
class PeerRun final : public conformance::Run {
 public:
  PeerRun(std::unique_ptr<imperivm::core::sim::GameSession> session,
          std::vector<WarTally>* log) noexcept
      : session_(std::move(session)), log_(log) {}
  PeerRun(const PeerRun&) = delete;
  PeerRun& operator=(const PeerRun&) = delete;
  ~PeerRun() override {
    if (log_ == nullptr) return;
    tally_.hash = session_->report().hash;
    log_->push_back(std::move(tally_));
  }

  [[nodiscard]] imperivm::core::sim::World& world() noexcept override {
    return session_->world();
  }
  void advance(std::int32_t turn_length) override {
    session_->advance(1, turn_length);
    if (log_ != nullptr) tally_.observe(*session_, turns());
  }
  /// Once the session is started or loaded: what the tally counts from.
  void baseline() {
    if (log_ != nullptr) tally_.baseline(*session_, turns());
  }

  [[nodiscard]] imperivm::core::sim::GameSession& session() noexcept { return *session_; }

 private:
  std::unique_ptr<imperivm::core::sim::GameSession> session_;
  std::vector<WarTally>* log_;
  WarTally tally_;
};

/// The session behind a run, or null for a run that is not a session's.
imperivm::core::sim::GameSession* session_of(conformance::Run& run) {
  auto* peer = dynamic_cast<PeerRun*>(&run);
  return peer == nullptr ? nullptr : &peer->session();
}

/// A real `GameSession` over a shipped map, as a conformance scenario.
///
/// This is the adapter `conformance.hpp` describes in "Attaching a
/// `GameSession`", and it is the whole of it: build the session, wrap it in
/// a `Run`, start the object scripts. `start()` is a pure function of
/// the seed because everything it reads -- the graph, the registry, the map
/// bytes -- is immutable for the scenario's lifetime.
class MapScenario final : public conformance::Scenario {
 public:
  /// `with_commands` gives the session `DATA/COMMANDS/*.XML`. Only the
  /// lockstep check asks for it, and deliberately: a session with the table
  /// resolves the verbs its queues name and launches their scripts, so
  /// turning it on for `imconform map` would change what every existing
  /// check simulates and how long it takes. The orders are this check's
  /// input; they are not a correction to the others.
  MapScenario(Installation& install, std::string name, bool run_scripts, bool with_commands = false)
      : install_(&install),
        name_(std::move(name)),
        run_scripts_(run_scripts),
        with_commands_(with_commands) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }

  /// Build a skirmish instead (see `Skirmish`): its inputs, the match started
  /// over `seats`, and a war tally kept per run.
  void set_skirmish(Skirmish* skirmish) noexcept { skirmish_ = skirmish; }
  [[nodiscard]] Skirmish* skirmish() const noexcept { return skirmish_; }
  /// Every run's tally, as each run ended. Only a skirmish keeps one.
  [[nodiscard]] std::vector<WarTally>& tallies() const noexcept { return tallies_; }

  /// Whose screen each started session is. `kNoPlayer` everywhere but the
  /// udp peers, which set their own seat -- the one value two peers of one
  /// match are guaranteed to disagree about, and so the one the peer
  /// comparison exists to prove never reaches hashed state.
  void set_local_player(imperivm::core::PlayerId player) noexcept { local_player_ = player; }

  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
    return start_as(seed, local_player_);
  }

  /// A peer's run from its own seat: `check_netplay` starts every peer so.
  [[nodiscard]] std::unique_ptr<conformance::Run> start_as(
      std::uint32_t seed, imperivm::core::PlayerId local) const override {
    auto session = imperivm::core::sim::GameSession::create(install_->registry, inputs(), seed);
    if (!session.ok()) return nullptr;
    auto run = std::make_unique<PeerRun>(std::move(session.value()),
                                         skirmish_ != nullptr ? &tallies_ : nullptr);
    imperivm::core::sim::GameSession& game = run->session();
    game.set_local_player(local);
    if (skirmish_ != nullptr) {
      // The app's order: the match, the objects' scripts, the AI.
      (void)game.start_match(match_options());
      (void)game.start_object_scripts();
      (void)game.start_ai();
    } else if (run_scripts_) {
      game.start_object_scripts();
      // And the AI, so that the determinism checks cover `DATA\AI` -- which
      // they did not for as long as nothing here started it.
      game.start_ai();
    }
    run->baseline();
    return run;
  }

  /// A late joiner's session: built as every peer's was, from the same
  /// inputs and seed, then put where the host's save left it.
  [[nodiscard]] std::unique_ptr<conformance::Run> resume(std::uint32_t seed,
                                                         std::span<const std::byte> save,
                                                         std::string_view map) const {
    return resume_as(seed, save, map, local_player_);
  }
  [[nodiscard]] std::unique_ptr<conformance::Run> resume_as(std::uint32_t seed,
                                                            std::span<const std::byte> save,
                                                            std::string_view map,
                                                            imperivm::core::PlayerId local) const {
    auto session = imperivm::core::sim::GameSession::create(install_->registry, inputs(), seed);
    if (!session.ok()) return nullptr;
    auto run = std::make_unique<PeerRun>(std::move(session.value()),
                                         skirmish_ != nullptr ? &tallies_ : nullptr);
    run->session().set_local_player(local);
    if (!run->session().load(save, map).ok()) return nullptr;
    run->baseline();
    return run;
  }

 private:
  [[nodiscard]] imperivm::core::sim::SessionInputs inputs() const {
    if (skirmish_ != nullptr) {
      imperivm::core::sim::SessionInputs inputs =
          imperivm::gamedata::session_inputs(skirmish_->install, skirmish_->payloads);
      inputs.scripts = skirmish_->scripts.get();
      return inputs;
    }
    imperivm::core::sim::SessionInputs inputs;
    inputs.map_objects = install_->container.map_objects;
    inputs.player_setup = install_->container.player_spans;
    inputs.constants = install_->constants;
    inputs.ai_profile = install_->ai_profile;
    if (with_commands_) inputs.commands = install_->command_spans;
    inputs.classes = &install_->graph;
    // Null isolates the systems from the VM, which `session.hpp` calls a
    // legitimate configuration and which is exactly what a bisection wants:
    // if `--no-scripts` is deterministic and the default is not, the
    // divergence is in the VM.
    inputs.scripts = run_scripts_ ? install_->scripts.get() : nullptr;
    return inputs;
  }

  /// See `Skirmish::map_setup`. The networked form is `netplay.hpp`'s
  /// `match_options` in the app, seat for seat.
  [[nodiscard]] imperivm::core::sim::MatchOptions match_options() const {
    imperivm::core::sim::MatchOptions options;
    if (skirmish_ != nullptr) options.starting_gold = skirmish_->gold;
    if (skirmish_ == nullptr || skirmish_->map_setup) return options;
    options.multiplayer = true;
    for (const imperivm::core::PlayerId seat : skirmish_->seats) {
      if (seat >= imperivm::core::sim::kPlayerCount) continue;
      options.control_set[seat] = true;
      options.controls[seat] = imperivm::core::sim::PlayerControl::human;
      if (options.human == imperivm::core::kNoPlayer || seat < options.human) options.human = seat;
    }
    return options;
  }

  Installation* install_;
  std::string name_;
  bool run_scripts_ = true;
  bool with_commands_ = false;
  imperivm::core::PlayerId local_player_ = imperivm::core::kNoPlayer;
  Skirmish* skirmish_ = nullptr;
  mutable std::vector<WarTally> tallies_;
};

/// The seats a skirmish's peers take by default: the lowest `count` players
/// that hold a stronghold once the match has started -- the players a
/// skirmish is between, whoever is human.
std::vector<imperivm::core::PlayerId> skirmish_seats(const MapScenario& scenario,
                                                     std::uint32_t seed, std::size_t count) {
  using namespace imperivm::core::sim;
  std::vector<imperivm::core::PlayerId> seats;
  std::unique_ptr<conformance::Run> probe = scenario.start(seed);
  if (probe == nullptr) return seats;
  if (EconomySystem* economy = economy_of(probe->world()); economy != nullptr) {
    for (const Settlement& settlement : economy->settlements().all()) {
      if (settlement.kind != SettlementKind::stronghold) continue;
      if (settlement.owner >= kNeutralWildlife) continue;
      seats.push_back(settlement.owner);
    }
  }
  std::sort(seats.begin(), seats.end());
  seats.erase(std::unique(seats.begin(), seats.end()), seats.end());
  if (seats.size() > count) seats.resize(count);
  // The probe is no peer: its tally goes before anybody reads the log.
  probe.reset();
  scenario.tallies().clear();
  return seats;
}

/// What the AI's war did, from the longest-running peer's tally, and whether
/// it did enough to have tested anything. `need_war` is the guard a skirmish
/// check stands on in place of the orders an adventure map's check drives:
/// no blow between two players means no army ever reached another's, and a
/// pass over that is a pass over an idle map.
bool print_war(const MapScenario& scenario, bool need_war) {
  const std::vector<WarTally>& tallies = scenario.tallies();
  const WarTally* longest = nullptr;
  for (const WarTally& tally : tallies) {
    if (longest == nullptr || tally.last_turn - tally.first_turn >
                                  longest->last_turn - longest->first_turn) {
      longest = &tally;
    }
  }
  if (longest == nullptr) {
    std::printf("war       FAIL  no run was tallied\n");
    return false;
  }
  std::printf("war       %zu blows and %zu deaths between players, the first at turns %llu and "
              "%llu; %zu captures, %zu of a stronghold%s\n",
              longest->blows, longest->kills,
              static_cast<unsigned long long>(longest->first_blow),
              static_cast<unsigned long long>(longest->first_kill), longest->captures,
              longest->strongholds_taken,
              longest->strongholds_taken == 0
                  ? ""
                  : (", the first at turn " + std::to_string(longest->first_stronghold_taken)).c_str());
  if (longest->blows != 0) std::printf("          the first, %s\n", longest->first_blow_by.c_str());
  std::size_t shown = 0;
  for (const std::string& event : longest->events) {
    if (shown++ == 40) {
      std::printf("          ... and %zu more\n", longest->events.size() - 40);
      break;
    }
    std::printf("          %s\n", event.c_str());
  }
  std::printf("session   hash %016llx after turn %llu\n",
              static_cast<unsigned long long>(longest->hash),
              static_cast<unsigned long long>(longest->last_turn));
  // Every run's screen, in the order the runs ended: a peer per seat, and
  // "none" for an unnetworked replay.
  std::printf("screens  ");
  for (const WarTally& tally : tallies) {
    if (tally.screen == imperivm::core::kNoPlayer) {
      std::printf(" none");
    } else {
      std::printf(" %u", static_cast<unsigned>(tally.screen));
    }
  }
  std::printf("\ncomputer  plays");
  for (const imperivm::core::PlayerId seat : longest->computer_seats) {
    std::printf(" %u", static_cast<unsigned>(seat));
  }
  std::printf(" from the start\n");
  if (need_war && longest->blows == 0) {
    std::printf("war       FAIL  no blow between two players: the war never started, so nothing "
                "was checked\n");
    return false;
  }
  return true;
}

// -- the subcommands -------------------------------------------------------

int print_report(const conformance::Report& report) {
  std::fputs(report.summary().c_str(), stdout);
  return report.ok() ? 0 : 1;
}

int run_self(std::size_t turns, std::int32_t length) {
  const std::unique_ptr<conformance::Scenario> scenario = conformance::make_reference_scenario();
  const std::vector<std::int32_t> schedule = conformance::uniform_schedule(length, turns);
  const conformance::Report report = conformance::run(*scenario, /*seed=*/0x2a, schedule);
  return print_report(report);
}

int print_trace(std::size_t turns, std::int32_t length) {
  const std::unique_ptr<conformance::Scenario> scenario = conformance::make_reference_scenario();
  const std::vector<std::int32_t> schedule = conformance::uniform_schedule(length, turns);
  const conformance::Trace trace = conformance::record(*scenario, /*seed=*/0x2a, schedule);
  const auto text = conformance::write_trace(trace);
  if (!text.ok()) {
    std::fprintf(stderr, "trace refused: a reserved hash channel is non-zero\n");
    return 1;
  }
  std::fputs(text.value().c_str(), stdout);
  return 0;
}

struct MapArgs {
  std::string game;
  std::string map;
  std::size_t turns = 50;
  std::int32_t length = 800;
  std::uint32_t seed = 1;
  std::string golden;
  bool record_golden = false;
  bool scripts = true;
  /// `--skirmish`: the map as a skirmish the AI plays (see `Skirmish`), the
  /// peers seats that give no orders. `--setup map` plays the map's own
  /// single-player setup instead of the networked one; `--peers N` seats N.
  bool skirmish = false;
  bool map_setup = false;
  std::size_t peers = 0;
  /// `--gold N`: the settings screen's starting gold, 2500, 5000 or 10000
  /// (-1, the default, is the map's own). A richer start trains armies
  /// sooner, so a war comes in fewer turns with nothing else changed.
  std::int32_t gold = -1;
  /// `--speed S`, netplay and netjoin: the match's game speed, per mille,
  /// from its first turn -- what reaches a war in fewer turns of a
  /// negotiated length. Zero is the protocol's default.
  std::int32_t speed = 0;
  /// `--seats A,B,...`: the peers' players, in place of the lowest that hold
  /// a stronghold -- to keep a seat the computer's whose play a check needs.
  std::vector<imperivm::core::PlayerId> seats;
  /// `--strike TURN`: the first seat orders a blow on that turn; see
  /// `strike_stream`. Zero orders none, and the war is the computer's.
  std::size_t strike = 0;
};

/// `--skirmish`'s half of a check: load the game as the app does, and seat
/// `count` peers (or `args.peers`) on the players a skirmish is between. A
/// check that needs no war between them asks for fewer with `least`.
bool prepare_skirmish(const MapArgs& args, MapScenario& scenario, Skirmish& skirmish,
                      std::size_t count, std::size_t least = 2) {
  if (!load_skirmish(args.game, args.map, skirmish)) return false;
  skirmish.map_setup = args.map_setup;
  skirmish.gold = args.gold;
  scenario.set_skirmish(&skirmish);
  // The probe that picks the seats runs the map's own setup: who holds a
  // stronghold is the map's, whoever turns out to be human.
  const bool map_setup = skirmish.map_setup;
  skirmish.map_setup = true;
  skirmish.seats = !args.seats.empty()
                       ? args.seats
                       : skirmish_seats(scenario, args.seed, args.peers != 0 ? args.peers : count);
  skirmish.map_setup = map_setup;
  std::printf("skirmish  %s setup; peers as players", map_setup ? "the map's own" : "a networked");
  for (const imperivm::core::PlayerId seat : skirmish.seats) {
    std::printf(" %u", static_cast<unsigned>(seat));
  }
  std::printf(", who give no orders; the computer plays %s",
              map_setup ? "every seat but the start player's" : "every other seat");
  if (skirmish.gold >= 0) std::printf("; starting gold %d", skirmish.gold);
  std::printf("\n");
  if (skirmish.seats.size() < least) {
    std::fprintf(stderr, "fewer than %zu players hold a stronghold: nothing to check\n", least);
    return false;
  }
  return true;
}

/// A scenario as one peer sees it: `start` is the base's `start_as` this seat.
class SeatedScenario final : public conformance::Scenario {
 public:
  SeatedScenario(const conformance::Scenario& base, imperivm::core::PlayerId seat)
      : base_(&base), seat_(seat) {}
  [[nodiscard]] std::string_view name() const noexcept override { return base_->name(); }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
    return base_->start_as(seed, seat_);
  }

 private:
  const conformance::Scenario* base_;
  imperivm::core::PlayerId seat_;
};

/// A deterministic command stream over a shipped map's own objects.
///
/// A lockstep check that drove no orders would pass for the wrong reason, so
/// the stream has to be real: actors the issuing player actually controls,
/// targets on the map, spread over the turns. It is built from a probe run of
/// the scenario -- started, read, discarded -- because the stream has to be
/// fixed *before* the two peers start, and both peers must be handed the same
/// one.
///
/// Every choice here is a function of the seed and the map. No clock, no
/// address, no iteration over anything unordered: the stream is part of the
/// input, and a stream that varied between runs would make the harness accuse
/// the simulation of the tool's bug.
imperivm::core::sim::CommandStream synthesise_stream(const conformance::Scenario& scenario,
                                                     std::uint32_t seed, std::size_t turns,
                                                     std::size_t& actors_found) {
  using namespace imperivm::core::sim;
  CommandStream stream;
  actors_found = 0;
  std::unique_ptr<conformance::Run> probe = scenario.start(seed);
  if (probe == nullptr) return stream;
  World& world = probe->world();
  // The same verifier a click uses. Without one every candidate carrying a
  // `verify=` script comes back `unknown`, and the resolver refuses to guess
  // -- which is right, and which would make a synthesised stream that drove
  // nothing.
  auto* session_run = session_of(*probe);
  std::unique_ptr<ScriptOrderVerifier> verifier;
  if (session_run != nullptr) {
    verifier = std::make_unique<ScriptOrderVerifier>(session_run->scheduler(),
                                                     session_run->host_context());
  }

  // One issuer, and the units it owns, in ascending object id -- which is the
  // order `World::objects()` is already in and the one order that does not
  // depend on how the map was walked.
  struct Owned {
    PlayerId owner;
    std::vector<ObjectId> units;
    std::vector<Point> places;
  };
  std::map<PlayerId, Owned> by_owner;
  for (const WorldObject& object : world.objects()) {
    if (object.state.owner == kNoPlayer) continue;
    if (object.state.health <= 0) continue;
    if (object.state.holder != kNoObject) continue;
    Owned& owned = by_owner[object.state.owner];
    owned.owner = object.state.owner;
    if (owned.places.size() < 16) owned.places.push_back(object.state.position);
    if (owned.units.size() >= 8) continue;
    // **Only actors that a right click on bare ground actually commands.**
    // Most of what a player owns is a building, and a building's
    // `<defaultcmd>` has no ground block -- so a stream built from "things
    // this player owns" resolves to nothing at all and the check passes
    // having driven an empty match. Asking the class graph the same question
    // the click asks is what keeps that from happening quietly.
    OrderTarget probe_target;
    probe_target.point = object.state.position;
    const DefaultOrder resolved =
        resolve_default_order(world, object.id, probe_target, false, verifier.get());
    if (resolved.status != DefaultOrderStatus::resolved) continue;
    owned.units.push_back(object.id);
  }

  // The two players with the most to command. Two rather than one because the
  // canonical order only means anything when more than one player issues.
  std::vector<const Owned*> issuers;
  for (const auto& [owner, owned] : by_owner) {
    if (owned.units.size() >= 2) issuers.push_back(&owned);
  }
  if (issuers.size() > 2) issuers.resize(2);
  if (issuers.empty()) return stream;

  stream.turns.resize(turns);
  std::uint32_t draw = seed | 1u;
  const auto next = [&draw]() {
    // A small xorshift, here and not from the world's RNG: drawing from the
    // world would change the simulation the check is measuring.
    draw ^= draw << 13;
    draw ^= draw >> 17;
    draw ^= draw << 5;
    return draw;
  };
  for (std::size_t turn = 0; turn < turns; ++turn) {
    // An order every fourth turn, so the match has quiet turns in it too --
    // a stream that is busy on every turn cannot catch a fold that ignores
    // the turn index.
    if (turn % 4 != 1) continue;
    std::uint32_t sequence = 0;
    for (const Owned* owned : issuers) {
      const std::size_t count = 1 + (next() % std::min<std::size_t>(3, owned->units.size()));
      NetOrder order;
      order.issuer = owned->owner;
      order.sequence = sequence++;
      for (std::size_t i = 0; i < count; ++i) {
        order.actors.push_back(owned->units[(next() + i) % owned->units.size()]);
      }
      order.target.point = owned->places[next() % owned->places.size()];
      order.mode = (next() & 1u) != 0 ? OrderMode::append : OrderMode::replace;
      order.modifier = (next() & 2u) != 0;
      actors_found += order.actors.size();
      stream.turns[turn].orders.push_back(std::move(order));
    }
  }
  return stream;
}

/// `--strike TURN`: a blow between two players that a peer orders, rather
/// than one the computer's timing brings on.
///
/// The short network war tests stood on the AI's war -- a blow between two
/// players was their proof that combat between players ran on every peer --
/// and the first blow on Crossroads moved with every faithful fix to the AI
/// or to movement: turn 261, then 736, then 405, then not inside the run,
/// three times in one day. A blow a peer orders waits on neither.
///
/// From `TURN` on, the first turn a seat has a unit that a right click on an
/// enemy's object sends to `attack`, the seat holding the nearest such pair
/// right-clicks that object with every unit of its that the click sends
/// (eight at most), as a player would; they walk to it and strike. Once.
///
/// **Chosen from the world, on the turn, not ahead of it.** A skirmish's units
/// are trained and spawned on the clock, so their ids follow the turn lengths,
/// which a network negotiates: an order fixed before the match named units
/// that, under another schedule, were not there. Every peer is asked on the
/// same turn and holds the same world, so every peer chooses the same order,
/// and only the seat that issues it sends it. Ascending ids and integer
/// distances; no clock, and no draw from the world's RNG.
struct Striker {
  std::size_t from = 0;  ///< zero strikes never
  std::vector<imperivm::core::PlayerId> seats;
  bool done = false;

  /// The order, on the first turn at or after `from` it is possible. Prints it.
  std::optional<imperivm::core::sim::NetOrder> take(conformance::Run& run, std::size_t turn) {
    using namespace imperivm::core::sim;
    if (from == 0 || done || turn < from) return std::nullopt;
    World& world = run.world();
    auto* session_run = session_of(run);
    std::unique_ptr<ScriptOrderVerifier> verifier;
    if (session_run != nullptr) {
      verifier = std::make_unique<ScriptOrderVerifier>(session_run->scheduler(),
                                                       session_run->host_context());
    }
    const PlayerTable& players = world.players();
    const auto live = [](const WorldObject& object) {
      return object.state.health > 0 && object.state.holder == kNoObject &&
             !object.state.flags.unspawned && object.class_index != imperivm::core::kNoClass;
    };
    const auto attacks = [&](ObjectId actor, const WorldObject& target) {
      OrderTarget aim;
      aim.object = target.id;
      aim.point = target.state.position;
      const DefaultOrder resolved = resolve_default_order(world, actor, aim, false, verifier.get());
      return resolved.status == DefaultOrderStatus::resolved && resolved.verb == "attack";
    };
    std::vector<const WorldObject*> units;
    for (const WorldObject& object : world.objects()) {
      if (!live(object) || !object.state.flags.is_unit) continue;
      if (std::find(seats.begin(), seats.end(), object.state.owner) != seats.end()) {
        units.push_back(&object);
      }
    }
    if (units.empty()) return std::nullopt;
    // Every (unit, enemy object) pair, nearest first; the first the click
    // makes an attack of decides.
    struct Pair {
      std::int64_t d2;
      const WorldObject* unit;
      const WorldObject* target;
    };
    std::vector<Pair> pairs;
    for (const WorldObject& object : world.objects()) {
      const PlayerId owner = object.state.owner;
      if (!live(object) || owner >= kNeutralWildlife || !PlayerTable::is_valid(owner)) continue;
      for (const WorldObject* unit : units) {
        if (owner == unit->state.owner) continue;
        if ((players.relation_word(unit->state.owner, owner) & 1u) != 0) continue;
        const std::int64_t dx = unit->state.position.x - object.state.position.x;
        const std::int64_t dy = unit->state.position.y - object.state.position.y;
        pairs.push_back({dx * dx + dy * dy, unit, &object});
      }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
      if (a.d2 != b.d2) return a.d2 < b.d2;
      if (a.unit->id != b.unit->id) return a.unit->id < b.unit->id;
      return a.target->id < b.target->id;
    });
    for (const Pair& pair : pairs) {
      if (!attacks(pair.unit->id, *pair.target)) continue;
      NetOrder order;
      order.issuer = pair.unit->state.owner;
      order.target.object = pair.target->id;
      order.target.point = pair.target->state.position;
      for (const WorldObject* unit : units) {
        if (order.actors.size() == 8) break;
        if (unit->state.owner == order.issuer && attacks(unit->id, *pair.target)) {
          order.actors.push_back(unit->id);
        }
      }
      done = true;
      const imperivm::core::ClassGraph* graph = world.class_graph();
      std::printf("strike    turn %zu: player %u's %zu unit(s) at player %u's %s %u, %lld world "
                  "units from the nearest\n",
                  turn, static_cast<unsigned>(order.issuer), order.actors.size(),
                  static_cast<unsigned>(pair.target->state.owner),
                  graph == nullptr ? "object"
                                   : std::string(graph->at(pair.target->class_index).id).c_str(),
                  static_cast<unsigned>(pair.target->id),
                  static_cast<long long>(std::sqrt(static_cast<double>(pair.d2))));
      std::fflush(stdout);
      return order;
    }
    return std::nullopt;
  }
};

/// The verifier a click has, over `run`'s own scripts; null without a session.
/// Without it a right click on an enemy is blocked -- `attack` has a
/// `verify=` -- and a strike would be an order that queued nothing.
std::unique_ptr<imperivm::core::sim::OrderVerifier> click_verifier(conformance::Run& run) {
  auto* session_run = session_of(run);
  if (session_run == nullptr) return nullptr;
  return std::make_unique<imperivm::core::sim::ScriptOrderVerifier>(session_run->scheduler(),
                                                                    session_run->host_context());
}

/// A peer that drives no stream of its own but strikes (`Striker`): the
/// orders it gives are kept, so that `netcmds` hashes them as a stream's.
class StrikeDriver final : public conformance::Driver {
 public:
  StrikeDriver(Striker striker, std::size_t turns) : striker_(std::move(striker)) {
    stream_.turns.resize(turns);
  }
  [[nodiscard]] std::uint64_t drive(conformance::Run& run, std::size_t turn) override {
    bool struck = false;
    if (turn < stream_.turns.size()) {
      if (std::optional<imperivm::core::sim::NetOrder> order = striker_.take(run, turn)) {
        stream_.turns[turn].orders.push_back(std::move(*order));
        struck = true;
      }
    }
    std::unique_ptr<imperivm::core::sim::OrderVerifier> verifier = click_verifier(run);
    imperivm::core::sim::StreamDriver inner(stream_, verifier.get());
    const std::uint64_t hash = inner.drive(run, turn);
    if (struck) {
      std::printf("strike    %zu command(s) queued, %zu blocked\n", inner.issued(), inner.blocked());
    }
    return hash;
  }

 private:
  Striker striker_;
  imperivm::core::sim::CommandStream stream_;
};

/// `Striker` for `check_netplay`'s peers, one a seat, and the click's
/// verifier for every run, the unnetworked replay's too.
class StrikeOrders final : public imperivm::core::sim::RunOrders,
                           public imperivm::core::sim::RunVerifiers {
 public:
  StrikeOrders(std::size_t from, std::vector<imperivm::core::PlayerId> seats)
      : from_(from), seats_(std::move(seats)) {}
  [[nodiscard]] std::vector<imperivm::core::sim::NetOrder> orders(
      imperivm::core::PlayerId player, std::size_t packet, conformance::Run& run) override {
    auto [it, fresh] = strikers_.try_emplace(player, Striker{from_, seats_});
    std::vector<imperivm::core::sim::NetOrder> out;
    if (std::optional<imperivm::core::sim::NetOrder> order = it->second.take(run, packet)) {
      out.push_back(std::move(*order));
    }
    return out;
  }
  [[nodiscard]] std::unique_ptr<imperivm::core::sim::OrderVerifier> verifier(
      conformance::Run& run) const override {
    return click_verifier(run);
  }

 private:
  std::size_t from_;
  std::vector<imperivm::core::PlayerId> seats_;
  std::map<imperivm::core::PlayerId, Striker> strikers_;
};

/// `imconform lockstep --skirmish`: two peers, no orders, the AI's war.
///
/// Each peer is started from its own seat (`start_as`), which is what a
/// lockstep peer is and what `check_lockstep` -- one scenario, run twice --
/// cannot express; so this records the two itself and compares them with the
/// same `compare_traces`. The guard is the war rather than the orders.
int run_lockstep_skirmish(const MapArgs& args, MapScenario& scenario) {
  using namespace imperivm::core::sim;
  const std::vector<imperivm::core::PlayerId>& seats = scenario.skirmish()->seats;
  const std::vector<std::int32_t> schedule =
      conformance::uniform_schedule(args.length, args.turns);
  std::printf("schedule  %zu turns of %d game-time units\n", args.turns, args.length);
  // A driver each: each peer chooses the strike from its own world.
  StrikeDriver driver_one(Striker{args.strike, seats}, args.turns);
  StrikeDriver driver_two(Striker{args.strike, seats}, args.turns);
  const SeatedScenario first(scenario, seats[0]);
  const SeatedScenario second(scenario, seats[1]);
  const conformance::Trace one =
      conformance::record(first, args.seed, conformance::Schedule(schedule), &driver_one);
  const conformance::Trace two =
      conformance::record(second, args.seed, conformance::Schedule(schedule), &driver_two);
  conformance::Divergence divergence = conformance::compare_traces(one, two);
  const std::string left = "peer as player " + std::to_string(seats[0]);
  const std::string right = "peer as player " + std::to_string(seats[1]);
  divergence.left = left;
  divergence.right = right;
  if (one.entries.size() != args.turns) {
    std::printf("lockstep  FAIL  the scenario could not be built\n");
    return 1;
  }
  if (divergence.diverged()) {
    std::printf("lockstep  FAIL  %s\n", divergence.describe().c_str());
    (void)print_war(scenario, false);
    return 1;
  }
  std::printf("lockstep  ok    two peers, as players %u and %u, agree on all %zu turns\n",
              static_cast<unsigned>(seats[0]), static_cast<unsigned>(seats[1]),
              one.entries.size());
  std::printf("hashes    %016llx after the last turn\n",
              static_cast<unsigned long long>(one.final_hashes().hash_of_hashes));
  return print_war(scenario, true) ? 0 : 1;
}

/// `imconform lockstep <game> <map> [turns] [length] [map-number]`: two peers,
/// one command stream, over a shipped map.
int run_lockstep(const MapArgs& args) {
  using namespace imperivm::core::sim;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;

  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  Skirmish skirmish;
  if (args.skirmish) {
    if (!prepare_skirmish(args, scenario, skirmish, 2)) return 1;
    return run_lockstep_skirmish(args, scenario);
  }
  const std::vector<std::int32_t> schedule =
      conformance::uniform_schedule(args.length, args.turns);

  std::size_t actors = 0;
  const CommandStream stream = synthesise_stream(scenario, args.seed, args.turns, actors);

  std::printf("classes   %zu from %zu files\n", install.graph.size(), install.class_files);
  std::printf("schedule  %zu turns of %d game-time units\n", args.turns, args.length);
  std::printf("scripts   %s\n", args.scripts ? "on" : "off");
  std::printf("stream    %zu orders over %zu turns, %zu actors named\n", stream.order_count(),
              stream.size(), actors);
  if (stream.order_count() == 0) {
    // A pass with nothing driven is a pass that means nothing, and this is
    // the one thing this subcommand must never report quietly.
    std::fprintf(stderr, "\nno orders could be synthesised: nothing to check\n"
                         "(a skirmish owns no units at turn 0: --skirmish plays the AI's war)\n");
    return 1;
  }

  StreamDriver driver(stream);
  conformance::Trace trace;
  const conformance::Divergence divergence = conformance::check_lockstep(
      scenario, args.seed, conformance::Schedule(schedule), driver, &trace);
  std::printf("driven    %zu orders applied, %zu commands queued (both peers)\n",
              driver.applied(), driver.issued());
  std::printf("refused   %zu actors refused, %zu unresolved, %zu blocked\n\n",
              driver.refused(), driver.unresolved(), driver.blocked());

  if (divergence.diverged()) {
    std::printf("lockstep  FAIL  %s\n", divergence.describe().c_str());
    return 1;
  }
  std::printf("lockstep  ok    two peers agree on all %zu turns\n", trace.entries.size());
  std::printf("netcmds   %016llx after the last turn\n",
              static_cast<unsigned long long>(trace.final_hashes().netcmds));
  return 0;
}

/// `imconform netplay <game> <map> [turns] [delay] [net-seed]`: every player
/// the synthesised stream names, each a peer with its own session, through the
/// turn negotiator over a loopback that delays, reorders and repeats every
/// packet.
///
/// `lockstep` hands two peers one stream. This hands each peer only its own
/// player's orders and lets the protocol assemble the turn -- so what it adds is
/// the wire, the input delay, the stalls and the negotiated length, and what it
/// proves is that none of them changes the game: the peers agree with each
/// other and with an unnetworked run of the stream they agreed on.
int run_netplay(const MapArgs& args, std::uint32_t delay, std::uint32_t net_seed) {
  using namespace imperivm::core::sim;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;

  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  Skirmish skirmish;
  if (args.skirmish && !prepare_skirmish(args, scenario, skirmish, 2)) return 1;
  std::size_t actors = 0;
  // A skirmish's peers give no orders: the stream is empty and the war is
  // the AI's.
  CommandStream intent;
  if (args.skirmish) {
    intent.turns.resize(args.turns);
  } else {
    intent = synthesise_stream(scenario, args.seed, args.turns, actors);
  }

  NetplayOptions options;
  options.seed = net_seed;
  options.lockstep.input_delay = delay;
  if (args.speed != 0) options.lockstep.game_speed = clamp_game_speed(args.speed);
  // Past the protocol's slack, whatever the delay: see `NetplayOptions`.
  options.max_delay = delay + 3;
  for (const NetTurn& turn : intent.turns) {
    for (const NetOrder& order : turn.orders) options.lockstep.peers.push_back(order.issuer);
  }
  if (args.skirmish) options.lockstep.peers = skirmish.seats;
  StrikeOrders strikes(args.strike, skirmish.seats);
  if (args.strike != 0) {
    options.players = &strikes;
    options.verifiers = &strikes;
  }
  std::sort(options.lockstep.peers.begin(), options.lockstep.peers.end());
  options.lockstep.peers.erase(
      std::unique(options.lockstep.peers.begin(), options.lockstep.peers.end()),
      options.lockstep.peers.end());

  std::printf("classes   %zu from %zu files\n", install.graph.size(), install.class_files);
  std::printf("scripts   %s\n", args.scripts ? "on" : "off");
  std::printf("intent    %zu orders over %zu turns, %zu actors named\n", intent.order_count(),
              intent.size(), actors);
  if ((intent.order_count() == 0 && !args.skirmish) || options.lockstep.peers.size() < 2) {
    // One peer is a match with nobody to disagree with, and it would pass.
    std::fprintf(stderr, "\nfewer than two players could be given orders: nothing to check\n");
    return 1;
  }
  std::printf("network   %zu peers, input delay %u, up to %u rounds late, every packet twice, seed %u\n",
              options.lockstep.peers.size(), delay, options.max_delay, net_seed);

  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, args.seed, args.turns, intent, options, &report);

  std::printf("wire      %zu deliveries, %zu bytes, %zu duplicates, %zu refused, %zu stalls\n",
              report.packets, report.bytes, report.duplicates, report.refused, report.stalls);
  std::printf("agreed    %zu orders, %zu commands queued (all peers)\n", report.orders,
              report.issued);
  if (!report.schedule.empty()) {
    const auto [lo, hi] = std::minmax_element(report.schedule.begin(), report.schedule.end());
    GameTime total = 0;
    for (const std::int32_t length : report.schedule) total += length;
    std::printf("schedule  %zu turns, %d..%d game-time units, %lld in all\n\n",
                report.schedule.size(), *lo, *hi, static_cast<long long>(total));
  }

  if (report.deadlocked) {
    std::printf("netplay   FAIL  deadlocked after %zu rounds, %zu turns in\n", report.rounds,
                report.turns);
    return 1;
  }
  if (divergence.diverged()) {
    std::printf("netplay   FAIL  %s\n", divergence.describe().c_str());
    if (args.skirmish) (void)print_war(scenario, false);
    return 1;
  }
  if (report.stalls == 0) {
    // The network never made a peer wait, so the path that makes lockstep
    // lockstep -- not running a turn you have not heard about -- never ran.
    std::printf("netplay   FAIL  no peer ever waited: the network was too kind to test anything\n");
    return 1;
  }
  if (report.refused != 0) {
    std::printf("netplay   FAIL  %zu packets refused on an honest network\n", report.refused);
    return 1;
  }
  if (args.skirmish && !print_war(scenario, true)) return 1;
  std::printf("netplay   ok    %zu peers agree on all %zu turns, and with the unnetworked run\n",
              options.lockstep.peers.size(), report.turns);
  return 0;
}

/// A session's save and load, for `check_netplay`'s late joiners, and its
/// sink, so that a seat really is handed to the computer and back.
class SessionSnapshots final : public imperivm::core::sim::RunSnapshots,
                               public imperivm::core::sim::RunSinks {
 public:
  SessionSnapshots(const MapScenario& scenario, std::string map)
      : scenario_(&scenario), map_(std::move(map)) {}
  [[nodiscard]] std::vector<std::byte> save(conformance::Run& run) const override {
    auto* session = session_of(run);
    if (session == nullptr) return {};
    auto bytes = session->save(map_);
    if (!bytes.ok()) return {};
    ++saves;
    largest = std::max(largest, bytes.value().size());
    return std::move(bytes.value());
  }
  [[nodiscard]] std::unique_ptr<conformance::Run> resume(
      std::uint32_t seed, std::span<const std::byte> bytes) const override {
    return scenario_->resume(seed, bytes, map_);
  }
  /// A late joiner's session from its own seat, as a real joiner's is.
  [[nodiscard]] std::unique_ptr<conformance::Run> resume_as(
      std::uint32_t seed, std::span<const std::byte> bytes,
      imperivm::core::PlayerId local) const override {
    return scenario_->resume_as(seed, bytes, map_, local);
  }
  [[nodiscard]] std::unique_ptr<imperivm::core::sim::NetCommandSink> sink(
      conformance::Run& run) const override {
    auto* session = session_of(run);
    if (session == nullptr) return nullptr;
    return std::make_unique<imperivm::core::sim::CommandBarSink>(*session);
  }
  mutable std::size_t saves = 0;
  mutable std::size_t largest = 0;

 private:
  const MapScenario* scenario_;
  std::string map_;
};

/// `imconform netjoin <game> <map> [turns] [net-seed]`: `netplay` with a
/// player who leaves and late joiners who take the seat -- this engine's late
/// join (`sim/netjoin.hpp`), on real sessions, in one process.
///
/// Every player the stream names plays, and one seat more that gives no
/// orders, all through the link layer on a star losing a fifth of its
/// datagrams. The busier non-host player leaves after 10 turns and the
/// computer takes its seat; after 18 the host seats a late joiner there from
/// its save, asking for speed 1400 in the last turn the save carries; that
/// joiner leaves after 10 turns of its own, and a second is seated at 36.
/// Every peer, every late joiner from its first turn, and an unnetworked
/// replay of the agreed history must agree on every channel -- takeovers and
/// hand-backs applied through the session's sink everywhere.
int run_netjoin(const MapArgs& args, std::uint32_t net_seed) {
  using namespace imperivm::core::sim;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;
  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  Skirmish skirmish;
  if (args.skirmish && !prepare_skirmish(args, scenario, skirmish, 3)) return 1;
  std::size_t actors = 0;
  CommandStream intent;
  std::vector<PlayerId> players;
  if (args.skirmish) {
    intent.turns.resize(args.turns);
    players = skirmish.seats;
  } else {
    intent = synthesise_stream(scenario, args.seed, args.turns, actors);
    for (const NetTurn& turn : intent.turns) {
      for (const NetOrder& order : turn.orders) players.push_back(order.issuer);
    }
  }
  std::sort(players.begin(), players.end());
  players.erase(std::unique(players.begin(), players.end()), players.end());
  if (players.size() < 2) {
    std::fprintf(stderr, "fewer than two players could be given orders: nothing to check\n");
    return 1;
  }
  const PlayerId leaver = players.back();
  for (PlayerId slot = 0; players.size() < 3 && slot < 8; ++slot) {
    if (std::find(players.begin(), players.end(), slot) == players.end()) players.push_back(slot);
  }
  std::sort(players.begin(), players.end());
  // When the seat changes hands. An adventure map's check is short and
  // fixed; a skirmish's is spread over the war, at the same proportions.
  std::size_t leave_at = 10;
  std::size_t first_join = 18;
  std::size_t stay = 10;
  std::size_t second_join = 36;
  if (args.skirmish) {
    leave_at = args.turns * 3 / 10;
    first_join = args.turns * 4 / 10;
    stay = args.turns / 10;
    second_join = args.turns * 6 / 10;
  }

  SessionSnapshots snapshots(scenario, args.map);
  NetplayOptions options;
  options.seed = net_seed;
  options.lockstep.peers = players;
  if (args.speed != 0) options.lockstep.game_speed = clamp_game_speed(args.speed);
  options.relay = true;
  options.loss_per_mille = 200;
  options.departures = {{leaver, leave_at, 3}};
  options.joins = {{leaver, first_join, 4, 1400, stay}, {leaver, second_join, 3, 0, 0}};
  options.snapshots = &snapshots;
  options.sinks = &snapshots;
  StrikeOrders strikes(args.strike, skirmish.seats);
  if (args.strike != 0) {
    options.players = &strikes;
    options.verifiers = &strikes;
  }

  std::printf("intent    %zu orders over %zu turns, %zu actors named\n", intent.order_count(),
              intent.size(), actors);
  std::printf("network   %zu peers, a star losing 200/1000, seed %u; player %u leaves and is "
              "joined twice\n",
              players.size(), net_seed, static_cast<unsigned>(leaver));
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, args.seed, args.turns, intent, options, &report);
  for (const auto& [peer, turn] : report.dropped) {
    std::printf("departed  player %u from turn %u\n", static_cast<unsigned>(peer), turn);
  }
  for (const auto& [peer, turn] : report.joined) {
    std::printf("joined    player %u from turn %u\n", static_cast<unsigned>(peer), turn);
  }
  std::printf("state     %zu saves, the largest %zu bytes\n", snapshots.saves, snapshots.largest);
  std::printf("agreed    %zu orders, %zu commands queued (all peers), %zu turns run by late "
              "joiners\n",
              report.orders, report.issued, report.joiner_turns);
  if (report.deadlocked) {
    std::printf("netjoin   FAIL  deadlocked after %zu rounds, %zu turns in\n", report.rounds,
                report.turns);
    return 1;
  }
  if (divergence.diverged()) {
    std::printf("netjoin   FAIL  %s\n", divergence.describe().c_str());
    if (args.skirmish) (void)print_war(scenario, false);
    return 1;
  }
  if (report.joined.size() != 2 || report.dropped.size() != 2 || report.refused != 0) {
    std::printf("netjoin   FAIL  %zu joined, %zu dropped, %zu refused\n", report.joined.size(),
                report.dropped.size(), report.refused);
    return 1;
  }
  if (args.skirmish && !print_war(scenario, true)) return 1;
  std::printf("netjoin   ok    %zu peers and 2 late joiners agree on every turn they ran, and "
              "with the unnetworked run\n",
              players.size());
  return 0;
}

struct UdpArgs {
  std::uint16_t port = 0;
  std::string host;
  std::uint32_t delay = imperivm::core::sim::kDefaultInputDelay;
  std::uint32_t drop = 0;
  std::uint32_t timeout_ms = 60000;
  /// Host: seat this many peers in all, adding seats that give no orders to
  /// the two the stream drives. Zero is the stream's players alone.
  std::size_t peers = 0;
  /// Leave the match once this many turns have run, saying so, and print the
  /// hashes of what was run. Zero stays to the end.
  std::size_t leave_after = 0;
  /// How long a silent link is waited for; zero is `NetMatch`'s default.
  std::uint32_t silence_ms = 0;
  /// Stop answering after this many turns, without a word: a crash.
  std::size_t crash_after = 0;
  /// Give a `set_speed` order with this peer's `speed_turn`-th packet, of
  /// `speed` per mille. Zero turns gives none.
  std::size_t speed_turn = 0;
  std::int32_t speed = 0;
  /// Host: once this many turns have run, run no more until a late joiner
  /// has been seated -- a test's way to keep the match going for a joiner
  /// that takes seconds to start. Zero never waits.
  std::size_t wait_join_at = 0;
  /// Host: ask for this speed with the first packet after seating a late
  /// joiner, which is the last turn the save carries. Zero asks for none.
  std::int32_t speed_on_join = 0;
  /// `--strike TURN` (`Striker`): both processes must be given it, as they
  /// are `--skirmish`. Zero strikes never.
  std::size_t strike = 0;
};

#if IMPERIVM_HAVE_NET
// -- udp-host / udp-join -----------------------------------------------------
//
// The same match `netplay` plays on a loopback, played by separate processes
// over a real socket: the lobby, the link layer's resends against real (and,
// with --drop, deliberate) loss, and the clock that measures round trips.
// Each process checks itself against an unnetworked run of the stream and
// schedule it agreed on, and prints the hashes a caller compares across
// processes -- which is the one comparison no single process can make.

std::uint64_t fnv64(std::span<const std::byte> data, std::uint64_t state = 1469598103934665603ULL) {
  for (const std::byte b : data) {
    state ^= static_cast<std::uint64_t>(b);
    state *= 1099511628211ULL;
  }
  return state;
}

/// What a joiner has to be simulating for the host to seat it: the game. The
/// map is checked separately, against the hash the start carries.
std::uint64_t content_hash(const Installation& install) { return fnv64(install.pack_bytes); }
std::uint64_t map_hash(const std::string& map) { return fnv64(read_file(map)); }

/// The orders `player` gives on its `k`-th turn.
std::vector<imperivm::core::sim::NetOrder> orders_of(const imperivm::core::sim::CommandStream& intent,
                                                     imperivm::core::PlayerId player,
                                                     std::size_t k) {
  std::vector<imperivm::core::sim::NetOrder> mine;
  if (const imperivm::core::sim::NetTurn* turn = intent.at(k); turn != nullptr) {
    for (const auto& order : turn->orders) {
      if (order.issuer == player) mine.push_back(order);
    }
  }
  return mine;
}

/// The headless session's sink, when the run is a session: surrenders and a
/// departed seat's takeover apply as they do in the app; command rows need a
/// bar and are not synthesised here.
std::optional<imperivm::core::sim::CommandBarSink> session_sink(conformance::Run& run) {
  auto* session_run = session_of(run);
  if (session_run == nullptr) return std::nullopt;
  return imperivm::core::sim::CommandBarSink(*session_run);
}

/// `StreamDriver` with that sink, so that the unnetworked replay of an agreed
/// history hands a departed seat to the computer on the turn the peer did.
/// No state between calls, as `conformance::Driver` requires.
///
/// A late joiner's replay starts where it joined: `first` is that turn, and
/// the run's own turn `k` is the match's `first + k`.
class SessionStreamDriver final : public conformance::Driver {
 public:
  /// `verify`: through the click's verifier, as a peer that strikes applies
  /// its agreed orders (`click_verifier`).
  explicit SessionStreamDriver(const imperivm::core::sim::CommandStream& stream,
                               std::size_t first = 0, bool verify = false)
      : stream_(&stream), first_(first), verify_(verify) {}
  [[nodiscard]] std::uint64_t drive(conformance::Run& run, std::size_t turn) override {
    if (const imperivm::core::sim::NetTurn* orders = stream_->at(first_ + turn); orders != nullptr) {
      std::optional<imperivm::core::sim::CommandBarSink> sink = session_sink(run);
      std::unique_ptr<imperivm::core::sim::OrderVerifier> verifier =
          verify_ ? click_verifier(run) : nullptr;
      (void)imperivm::core::sim::apply_turn(run.world(), *orders, verifier.get(),
                                            sink.has_value() ? &*sink : nullptr);
    }
    return imperivm::core::sim::stream_hash(*stream_, first_ + turn + 1);
  }

 private:
  const imperivm::core::sim::CommandStream* stream_;
  std::size_t first_;
  bool verify_;
};

/// The scenario a late joiner's replay starts from: the host's save.
class ResumedScenario final : public conformance::Scenario {
 public:
  ResumedScenario(const MapScenario& base, std::span<const std::byte> save, std::string map)
      : base_(&base), save_(save.begin(), save.end()), map_(std::move(map)) {}
  [[nodiscard]] std::string_view name() const noexcept override { return base_->name(); }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
    return base_->resume(seed, save_, map_);
  }

 private:
  const MapScenario* base_;
  std::vector<std::byte> save_;
  std::string map_;
};

int play_udp(MapScenario& scenario, imperivm::net::UdpSocket& socket,
             const imperivm::net::Lobby& lobby, const UdpArgs& udp) {
  using namespace imperivm::core::sim;
  namespace net = imperivm::net;
  const Start& start = lobby.start;
  // A late joiner (`sim/netjoin.hpp`, this engine's): it runs from the turn
  // it was seated at, from the host's save.
  const bool late = lobby.join.has_value();
  const std::uint32_t first_turn = late ? lobby.join->from_turn : 0;
  std::size_t actors = 0;
  // A skirmish's seats are the match's, as the host's start names them, and
  // its peers give no orders.
  CommandStream intent;
  if (Skirmish* skirmish = scenario.skirmish(); skirmish != nullptr) {
    skirmish->seats = start.config.peers;
    // The host's rule, not this process's flags: a joiner is told it.
    skirmish->gold = start.rules.starting_gold;
    intent.turns.resize(start.turns);
  } else {
    intent = synthesise_stream(scenario, start.seed, start.turns, actors);
  }
  // After the stream: every peer synthesises it from a headless probe, so
  // that it cannot depend on whose screen this is.
  scenario.set_local_player(start.you);

  std::printf("peer      player %u of", static_cast<unsigned>(start.you));
  for (const auto player : start.config.peers) std::printf(" %u", static_cast<unsigned>(player));
  std::printf(", match %08x, seed %u, delay %u, drop %u/1000\n",
              late ? lobby.join->match : start.match, start.seed, start.config.input_delay,
              udp.drop);
  if (late) {
    std::printf("late      as player %u from turn %u, speed %d, a save of %u bytes to come\n",
                static_cast<unsigned>(start.you), first_turn, start.config.game_speed,
                lobby.join->size);
  }
  std::fflush(stdout);

  net::NetMatch match(socket, lobby);
  match.set_drop(udp.drop, start.match ^ (0x9e3779b9u * (start.you + 1u)));
  if (udp.silence_ms != 0) match.set_silence_limit(udp.silence_ms);
  // Each agreed departure once, as it is agreed.
  std::set<imperivm::core::PlayerId> departures_said;
  const auto say_departures = [&] {
    for (const net::NetDeparture& gone : match.departures()) {
      if (!gone.from_turn.has_value() || !departures_said.insert(gone.player).second) continue;
      std::printf("departed  player %u (%s) from turn %u\n", static_cast<unsigned>(gone.player),
                  describe(gone.reason), *gone.from_turn);
      std::fflush(stdout);
    }
  };
  // A late joiner's world comes with its state.
  std::unique_ptr<conformance::Run> run = late ? nullptr : scenario.start(start.seed);
  if (run == nullptr && !late) {
    std::fprintf(stderr, "the scenario could not be built\n");
    return 1;
  }
  std::vector<std::byte> state;
  conformance::Trace trace;
  trace.scenario = std::string(scenario.name());
  trace.seed = start.seed;
  if (run != nullptr) {
    for (const System* system : run->world().systems()) trace.systems.emplace_back(system->name());
  }
  // Late joins, as this peer learns of them, and the host's side of each.
  std::set<std::pair<imperivm::core::PlayerId, std::uint32_t>> joins_said;
  bool seating_said = false;
  std::size_t seated_said = 0;
  // The joiner whose seating the speed was asked for, by its first turn.
  std::optional<std::uint32_t> speed_asked_for;
  const auto say_joins = [&] {
    for (const Admit& admit : match.node().admits()) {
      if (!joins_said.insert({admit.peer, admit.from_turn}).second) continue;
      std::printf("joining   player %u from turn %u\n", static_cast<unsigned>(admit.peer),
                  admit.from_turn);
    }
    if (const auto& seating = match.seating(); seating.has_value() && !seating_said) {
      seating_said = true;
      std::printf("admitted  player %u (%s) from turn %u\n", static_cast<unsigned>(seating->seat),
                  seating->name.c_str(), seating->from_turn);
    } else if (!seating.has_value()) {
      seating_said = false;
    }
    for (; seated_said < match.seated().size(); ++seated_said) {
      const net::NetMatch::Seating& done = match.seated()[seated_said];
      std::printf("seated    player %u from turn %u, %u bytes of state acknowledged\n",
                  static_cast<unsigned>(done.seat), done.from_turn, done.acked);
    }
    std::fflush(stdout);
  };

  // The k-th packet a peer sends carries the stream's k-th turn of orders; a
  // late joiner's first is the one for its first turn.
  std::size_t submitted = first_turn;
  // Every peer asks, from its world as it builds the packet; the seat whose
  // order it is sends it.
  Striker striker{udp.strike, start.config.peers};
  std::size_t issued = 0;
  // This peer's orders for its k-th packet: the stream's, and the speed.
  const auto mine = [&](std::size_t k) {
    std::vector<NetOrder> orders = orders_of(intent, start.you, k);
    if (run != nullptr) {
      if (std::optional<NetOrder> strike = striker.take(*run, k);
          strike.has_value() && strike->issuer == start.you) {
        orders.push_back(std::move(*strike));
      }
    }
    if (udp.speed_turn != 0 && k == udp.speed_turn) {
      NetOrder order;
      order.kind = NetOrderKind::set_speed;
      order.speed = udp.speed;
      orders.push_back(order);
    }
    // Checked at every packet, not once a loop: the hello that seats a
    // joiner can arrive inside `submit`'s own pump, after which the next
    // packet is for the last turn the save carries.
    if (const auto& seating = match.seating();
        udp.speed_on_join != 0 && seating.has_value() && speed_asked_for != seating->from_turn) {
      speed_asked_for = seating->from_turn;
      NetOrder order;
      order.kind = NetOrderKind::set_speed;
      order.speed = udp.speed_on_join;
      orders.push_back(order);
    }
    return orders;
  };
  const auto reached = [&] { return first_turn + trace.entries.size(); };
  const std::uint32_t begun = net::now_ms();
  (void)match.submit(mine(submitted++), begun);
  std::optional<std::uint32_t> finished_at;
  for (;;) {
    const std::uint32_t now = net::now_ms();
    match.pump(now);
    say_joins();
    // The host: a late joiner's state, once the turn before its first has run.
    if (match.wants_state().has_value() && run != nullptr) {
      auto* session = session_of(*run);
      auto save = session->save(start.map);
      if (!save.ok()) {
        std::printf("udp       FAIL  the session would not save for a late joiner\n");
        return 1;
      }
      std::printf("state     %zu bytes for player %u, after turn %u\n", save.value().size(),
                  static_cast<unsigned>(match.seating()->seat), *match.wants_state() - 1);
      std::fflush(stdout);
      match.provide_state(std::move(save.value()), now);
    }
    // The late joiner: its world arrives.
    if (match.awaiting_state()) {
      if (const std::optional<std::span<const std::byte>> held = match.state()) {
        run = scenario.resume(start.seed, *held, start.map);
        if (run == nullptr) {
          std::printf("udp       FAIL  the host's save did not load\n");
          return 1;
        }
        state.assign(held->begin(), held->end());
        for (const System* system : run->world().systems()) trace.systems.emplace_back(system->name());
        match.state_loaded();
        std::printf("loaded    %zu bytes, turn %llu, hashes verified\n", state.size(),
                    static_cast<unsigned long long>(run->turns()));
        std::fflush(stdout);
      }
    }
    // A test's host holds the match for a joiner still starting up.
    const bool holding = udp.wait_join_at != 0 && reached() >= udp.wait_join_at &&
                         match.seated().empty() && !match.seating().has_value();
    while (run != nullptr && !holding && reached() < start.turns) {
      std::optional<AgreedTurn> turn = match.take();
      if (!turn.has_value()) break;
      std::optional<CommandBarSink> sink = session_sink(*run);
      std::unique_ptr<OrderVerifier> verifier = udp.strike != 0 ? click_verifier(*run) : nullptr;
      issued += apply_turn(run->world(), turn->orders, verifier.get(),
                           sink.has_value() ? &*sink : nullptr).issued;
      for (const NetOrder& order : turn->orders.orders) {
        if (order.kind == NetOrderKind::set_speed) {
          std::printf("speed     player %u set %d on turn %u, from turn %u\n",
                      static_cast<unsigned>(order.issuer), clamp_game_speed(order.speed), turn->index,
                      turn->index + 1);
        }
        if (order.kind != NetOrderKind::departed && order.kind != NetOrderKind::joined) continue;
        const AiSystem* ai = ai_system_of(run->world());
        const AiPlayer* seat = ai == nullptr ? nullptr : ai->player_ai(order.issuer);
        const bool active = seat != nullptr && seat->active;
        if (order.kind == NetOrderKind::joined) {
          std::printf("handback  player %u on turn %u: %s\n", static_cast<unsigned>(order.issuer),
                      turn->index, active ? "the computer plays on" : "the computer stops");
          continue;
        }
        std::printf("takeover  player %u on turn %u: %s\n", static_cast<unsigned>(order.issuer),
                    turn->index, active ? "the computer takes the seat" : "the seat stays idle");
      }
      run->advance(turn->length);
      conformance::TraceEntry entry;
      entry.turn = run->turns();
      entry.length = turn->length;
      entry.time = run->time();
      entry.hashes = run->hashes();
      entry.hashes.netcmds =
          stream_hash(match.negotiator().history(), match.negotiator().history().size());
      entry.hashes.hash_of_hashes = conformance::roll_up(entry.hashes);
      trace.entries.push_back(entry);
      if (udp.crash_after != 0 && trace.entries.size() >= udp.crash_after) {
        // Gone without a word: nothing sent, nothing answered, from here.
        std::printf("udp       crashed after %zu turns\n", trace.entries.size());
        return 0;
      }
      if (udp.leave_after != 0 && trace.entries.size() >= udp.leave_after) break;
      if (reached() < start.turns) {
        // The clock read now, not at the top of the loop: `submit` pumps, and
        // what that pump hears is stamped with this time. A peer catching up
        // runs turn after turn here -- 31 turns in 14.9 s under a loaded
        // machine -- and stamping them all with the loop's first reading
        // made the next pump find the other peer 15 s silent, which it had
        // been answering the whole time.
        (void)match.submit(mine(submitted++), net::now_ms());
      }
    }
    if (udp.leave_after != 0 && trace.entries.size() >= udp.leave_after) {
      match.leave();
      std::uint64_t world = 1469598103934665603ULL;
      for (const conformance::TraceEntry& left : trace.entries) {
        for (int shift = 0; shift < 64; shift += 8) {
          world ^= (left.hashes.hash_of_hashes >> shift) & 0xFF;
          world *= 1099511628211ULL;
        }
      }
      std::printf("hashes    world %016llx netcmds %016llx\n", static_cast<unsigned long long>(world),
                  static_cast<unsigned long long>(trace.final_hashes().netcmds));
      std::printf("udp       left after %zu turns\n", trace.entries.size());
      return 0;
    }
    say_departures();
    if (reached() >= start.turns) {
      if (!finished_at.has_value()) finished_at = now;
      // Stay until everything this peer holds has been acknowledged -- a hub
      // that left first would strand the others mid-relay -- and a little
      // longer, so the last acknowledgements get out. Not forever: a peer
      // that already left acknowledges nothing.
      const std::uint32_t lingered = now - *finished_at;
      if ((match.node().held() == 0 && lingered >= 250) || lingered >= 3000) break;
    }
    if (const auto ended = match.ended(); ended.has_value() && reached() < start.turns) {
      std::printf("udp       FAIL  the match ended: %s, after %zu of %u turns\n",
                  describe(*ended), trace.entries.size(), start.turns);
      for (const net::NetDeparture& gone : match.departures()) {
        std::printf("          player %u left (%s)\n", static_cast<unsigned>(gone.player),
                    describe(gone.reason));
      }
      return 1;
    }
    if (now - begun > udp.timeout_ms) {
      std::printf("udp       FAIL  timed out after %zu of %u turns\n", trace.entries.size(),
                  start.turns);
      return 1;
    }
    socket.wait(5);
  }

  const net::MatchStats& stats = match.stats();
  const std::vector<std::int32_t>& schedule = match.negotiator().schedule();
  std::printf("wire      %zu sent, %zu dropped, %zu received, %zu duplicates, %zu refused, %zu strangers, %zu late\n",
              stats.sent, stats.dropped, stats.received, stats.duplicates, stats.refused,
              stats.strangers, stats.late);
  std::printf("agreed    %zu orders, %zu commands queued\n",
              match.negotiator().history().order_count(), issued);
  const auto [lo, hi] = std::minmax_element(schedule.begin(), schedule.end());
  std::printf("schedule  %zu turns, %d..%d game-time units\n", schedule.size(), *lo, *hi);
  std::uint64_t world = 1469598103934665603ULL;
  for (const conformance::TraceEntry& entry : trace.entries) {
    for (int shift = 0; shift < 64; shift += 8) {
      world ^= (entry.hashes.hash_of_hashes >> shift) & 0xFF;
      world *= 1099511628211ULL;
    }
  }
  std::printf("hashes    world %016llx netcmds %016llx\n", static_cast<unsigned long long>(world),
              static_cast<unsigned long long>(trace.final_hashes().netcmds));
  // Every turn's rolled-up hash, from the first this peer ran: what a caller
  // compares a late joiner with the others by, turn for turn.
  std::printf("trace     from %u:", first_turn);
  for (const conformance::TraceEntry& entry : trace.entries) {
    std::printf(" %016llx", static_cast<unsigned long long>(entry.hashes.hash_of_hashes));
  }
  std::printf("\n");

  // The unnetworked replay of what was agreed: from turn 0, or for a late
  // joiner from the host's save.
  const ResumedScenario resumed(scenario, state, start.map);
  const conformance::Scenario& replayed =
      late ? static_cast<const conformance::Scenario&>(resumed) : scenario;
  SessionStreamDriver driver(match.negotiator().history(), first_turn, udp.strike != 0);
  const conformance::Trace direct =
      conformance::record(replayed, start.seed, conformance::Schedule(schedule), &driver);
  conformance::Divergence divergence = conformance::compare_traces(direct, trace);
  if (divergence.diverged()) {
    divergence.left = "without a network";
    divergence.right = "this peer";
    std::printf("udp       FAIL  %s\n", divergence.describe().c_str());
    return 1;
  }
  if (stats.refused != 0) {
    std::printf("udp       FAIL  %zu refused on an honest network\n", stats.refused);
    return 1;
  }
  // A late joiner's tally starts at its seat, and the war may be behind it.
  if (scenario.skirmish() != nullptr && !print_war(scenario, !late)) return 1;
  std::printf("udp       ok    %zu turns, and the same as the unnetworked run\n",
              trace.entries.size());
  return 0;
}

int run_udp_host(const MapArgs& args, const UdpArgs& udp) {
  using namespace imperivm::core::sim;
  namespace net = imperivm::net;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;
  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  Skirmish skirmish;

  // The seats are the players the synthesised stream gives orders to: the
  // same stream every peer will synthesise from the same seed and map. A
  // skirmish's are the players it is between, and give no orders.
  std::vector<imperivm::core::PlayerId> players;
  if (args.skirmish) {
    if (!prepare_skirmish(args, scenario, skirmish, std::max<std::size_t>(2, udp.peers))) return 1;
    players = skirmish.seats;
  } else {
    std::size_t actors = 0;
    const CommandStream intent = synthesise_stream(scenario, args.seed, args.turns, actors);
    for (const NetTurn& turn : intent.turns) {
      for (const NetOrder& order : turn.orders) players.push_back(order.issuer);
    }
  }
  std::sort(players.begin(), players.end());
  players.erase(std::unique(players.begin(), players.end()), players.end());
  if (players.size() < 2) {
    std::fprintf(stderr, "fewer than two players could be given orders: nothing to check\n");
    return 1;
  }
  // Seats that give no orders, up to `--peers`: the lowest player slots the
  // stream does not use.
  for (imperivm::core::PlayerId slot = 1; players.size() < udp.peers && slot <= 8; ++slot) {
    if (std::find(players.begin(), players.end(), slot) == players.end()) players.push_back(slot);
  }
  std::sort(players.begin(), players.end());

  std::string error;
  std::optional<net::UdpSocket> socket = net::UdpSocket::open(udp.port, false, &error);
  if (!socket.has_value()) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  Start base;
  base.match = net::now_ms() ^ (static_cast<std::uint32_t>(socket->port()) << 16);
  base.you = players.front();
  base.seed = args.seed;
  base.turns = static_cast<std::uint32_t>(args.turns);
  base.config.peers = players;
  base.config.input_delay = udp.delay;
  base.map = args.map;
  base.map_hash = map_hash(args.map);
  base.rules.starting_gold = args.gold;
  const std::vector<imperivm::core::PlayerId> seats(players.begin() + 1, players.end());
  // The line a caller reads to learn the port, so it goes out at once.
  std::printf("listening port %u, waiting for %zu\n", static_cast<unsigned>(socket->port()),
              seats.size());
  std::fflush(stdout);

  const net::Lobby lobby =
      net::host_lobby(*socket, base, seats, content_hash(install), udp.timeout_ms);
  if (!lobby.ok) {
    std::printf("udp       FAIL  %s\n", lobby.error.c_str());
    return 1;
  }
  for (const auto& [player, at] : lobby.links) {
    std::printf("seated    player %u at %s\n", static_cast<unsigned>(player), at.str().c_str());
  }
  return play_udp(scenario, *socket, lobby, udp);
}

int run_udp_join(const MapArgs& args, const UdpArgs& udp) {
  using namespace imperivm::core::sim;
  namespace net = imperivm::net;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;
  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  // A skirmish's seats come in the host's start; see `play_udp`.
  Skirmish skirmish;
  if (args.skirmish) {
    if (!load_skirmish(args.game, args.map, skirmish)) return 1;
    skirmish.map_setup = args.map_setup;
    scenario.set_skirmish(&skirmish);
  }

  const std::optional<net::Endpoint> host = net::resolve(udp.host, 0);
  if (!host.has_value() || host->port == 0) {
    std::fprintf(stderr, "cannot resolve '%s' (host:port)\n", udp.host.c_str());
    return 2;
  }
  std::string error;
  std::optional<net::UdpSocket> socket = net::UdpSocket::open(0, false, &error);
  if (!socket.has_value()) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  Hello hello;
  hello.content = content_hash(install);
  hello.name = "imconform";
  const net::Lobby lobby = net::join_lobby(*socket, *host, hello, udp.timeout_ms);
  if (!lobby.ok) {
    std::printf("udp       FAIL  %s\n", lobby.error.c_str());
    return 1;
  }
  if (lobby.join.has_value()) {
    std::printf("joined    as player %u from turn %u\n", static_cast<unsigned>(lobby.start.you),
                lobby.join->from_turn);
  }
  if (lobby.start.map_hash != map_hash(args.map)) {
    // Say why before going: the host is otherwise left waiting on a peer
    // that will never send a turn.
    imperivm::core::sim::Refuse refuse;
    refuse.reason = imperivm::core::sim::Refuse::Reason::map;
    for (int copy = 0; copy < 2; ++copy) (void)socket->send(*host, encode(refuse));
    std::printf("udp       FAIL  refused: %s\n", describe(refuse.reason));
    return 1;
  }
  return play_udp(scenario, *socket, lobby, udp);
}
#endif  // IMPERIVM_HAVE_NET

/// `imconform observe <game> <map> [turns]`: looking is not an input.
///
/// A lockstep peer's interface runs scripts that no other peer runs: every
/// refresh of the command bar runs each row's `groupverifier`, the info bar
/// runs its own, and the cursor resolves a default order through the
/// `verify=` scripts. They run "off to the side", not on the scheduler, but
/// they borrow the world's list pool and can call any host function, and one
/// that drew from the RNG or wrote a counter would desync a match the moment
/// one player moved the mouse.
///
/// So two sessions from one seed and one local player, and only one of them
/// is watched: every turn it selects some of that player's units -- which
/// records when each was selected, a value one shipped script reads -- and
/// describes the command bar and the info bar and resolves the cursor over
/// the map, all for real. Then the per-turn hashes are compared. Everything
/// the watched run did is legitimately different on another peer, so any
/// difference is a leak.
int run_observe(const MapArgs& args) {
  using namespace imperivm::core::sim;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;
  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  // A skirmish is watched from its first peer's seat, through the war.
  Skirmish skirmish;
  if (args.skirmish && !prepare_skirmish(args, scenario, skirmish, 2)) return 1;

  // The player who owns the most, as `synthesise_stream` picks its issuers: a
  // watched player with nothing to select tests nothing. Buildings count --
  // they have command rows, and a row's verifier is what is being tested.
  std::unique_ptr<conformance::Run> probe = scenario.start(args.seed);
  if (probe == nullptr) {
    std::fprintf(stderr, "the scenario could not be built\n");
    return 1;
  }
  std::map<imperivm::core::PlayerId, std::size_t> owned;
  for (const WorldObject& object : probe->world().objects()) {
    if (object.state.owner == imperivm::core::kNoPlayer || object.state.health <= 0) continue;
    ++owned[object.state.owner];
  }
  probe.reset();
  scenario.tallies().clear();  // the probe watched nothing
  imperivm::core::PlayerId watched = imperivm::core::kNoPlayer;
  std::size_t most = 0;
  for (const auto& [player, count] : owned) {
    if (count > most) {
      most = count;
      watched = player;
    }
  }
  if (args.skirmish) {
    watched = skirmish.seats.front();
    most = owned[watched];
  }
  if (watched == imperivm::core::kNoPlayer) {
    std::fprintf(stderr, "no player owns anything: nothing to look at\n");
    return 1;
  }
  scenario.set_local_player(watched);

  const auto record_one = [&](bool look, std::size_t& looks, std::size_t& buttons,
                              std::size_t& resolved) {
    conformance::Trace trace;
    std::unique_ptr<conformance::Run> run = scenario.start(args.seed);
    if (run == nullptr) return trace;
    StrikeDriver strike(Striker{args.strike, skirmish.seats}, args.turns);
    for (const System* system : run->world().systems()) trace.systems.emplace_back(system->name());
    auto* session_run = session_of(*run);
    std::unique_ptr<CommandBar> cmdbar;
    std::unique_ptr<InfoBar> infobar;
    if (look && session_run != nullptr) {
      cmdbar = std::make_unique<CommandBar>(*session_run);
      infobar = std::make_unique<InfoBar>(*session_run, install.skills,
                                          install.unit_specials);
    }
    for (std::size_t turn = 0; turn < args.turns; ++turn) {
      if (cmdbar != nullptr) {
        GameSession& session = *session_run;
        World& world = session.world();
        // A different handful each turn, the way a player's clicks wander.
        std::vector<ObjectId> mine;
        for (const WorldObject& object : world.objects()) {
          if (object.state.owner == watched && object.state.health > 0) mine.push_back(object.id);
        }
        if (args.skirmish && !mine.empty()) {
          // A bar offers the rows every selected object shares, and a
          // skirmish player's objects are a town, its houses and its
          // garrison: a handful of all of them shares nothing. So one class
          // a turn, a different one each turn.
          const std::uint32_t kind = world.find(mine[(turn * 5) % mine.size()])->class_index;
          std::erase_if(mine, [&](ObjectId id) { return world.find(id)->class_index != kind; });
        }
        std::vector<ObjectId> pick;
        for (std::size_t i = 0; i < mine.size() && pick.size() < 8; ++i) {
          pick.push_back(mine[(i * 7 + turn * 3) % mine.size()]);
        }
        session.selections().assign(watched, pick);
        buttons += cmdbar->describe(watched).size();
        (void)infobar->describe(watched);
        ScriptOrderVerifier verifier(session.scheduler(), session.host_context());
        for (const ObjectId actor : pick) {
          OrderTarget cursor;
          const WorldObject* at = world.find(mine[(turn * 5) % mine.size()]);
          if (at != nullptr) cursor.point = at->state.position;
          if (resolve_default_order(world, actor, cursor, false, &verifier).status ==
              DefaultOrderStatus::resolved) {
            ++resolved;
          }
        }
        ++looks;
      }
      // The strike, on both runs alike: an order is an input, looking is not.
      if (args.strike != 0) (void)strike.drive(*run, turn);
      run->advance(args.length);
      conformance::TraceEntry entry;
      entry.turn = run->turns();
      entry.length = args.length;
      entry.time = run->time();
      entry.hashes = run->hashes();
      trace.entries.push_back(entry);
    }
    return trace;
  };

  std::size_t looks = 0;
  std::size_t buttons = 0;
  std::size_t resolved = 0;
  std::size_t unused = 0;
  const conformance::Trace plain = record_one(false, unused, unused, unused);
  const conformance::Trace watched_trace = record_one(true, looks, buttons, resolved);
  std::printf("watched   player %u, %zu objects at turn 0\n", static_cast<unsigned>(watched), most);
  std::printf("looked    %zu turns, %zu buttons described, %zu cursor orders resolved\n", looks,
              buttons, resolved);
  if (looks == 0 || buttons == 0) {
    std::fprintf(stderr, "\nnothing was described: nothing to check\n");
    return 1;
  }
  conformance::Divergence divergence = conformance::compare_traces(plain, watched_trace);
  if (divergence.diverged()) {
    divergence.left = "unwatched";
    divergence.right = "watched";
    std::printf("observe   FAIL  %s\n", divergence.describe().c_str());
    return 1;
  }
  if (args.skirmish && !print_war(scenario, true)) return 1;
  std::printf("observe   ok    %zu turns, watching changed no hash\n", plain.entries.size());
  return 0;
}

int run_map(const MapArgs& args) {
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;

  MapScenario scenario(install, args.map, args.scripts);
  const std::vector<std::int32_t> schedule =
      conformance::uniform_schedule(args.length, args.turns);

  std::printf("classes   %zu from %zu files\n", install.graph.size(), install.class_files);
  std::printf("host      %zu of %zu entry points implemented\n", install.implemented,
              install.registry.size());
  std::printf("schedule  %zu turns of %d game-time units (%lld total)\n", args.turns,
              args.length,
              static_cast<long long>(conformance::schedule_time(schedule)));
  std::printf("scripts   %s\n\n", args.scripts ? "on" : "off");

  conformance::Options options;
  // **Partition invariance is not a property this engine has once scripts run,
  // and asserting it would send someone hunting a bug that is the design.**
  //
  // `Scheduler::advance` runs each script at its own wake time now, so a
  // script due at 1000 runs at 1000 under 800-unit turns and under 400s alike.
  // But the world it reads stands at the turn's end -- 1600 under 800s, 1200
  // under 400s -- and a script spawned by a system, or one that waited zero,
  // runs at the turn's end. **What a script sees, and so what it does next,
  // therefore depends on the partition** -- and shipped scripts draw from
  // the world RNG when they wake (`CROW_IDLE.VS` sleeps for
  // `rand(2000)`, `DEER_IDLE.VS` has nine `rand` sites). A different number of
  // draws desynchronises the shared generator, and with it everything
  // downstream.
  //
  // Lockstep does not need this invariant. Every peer in a match runs the same
  // turn length, negotiated by the engine, so what has to hold is that the same
  // partition gives the same result -- which is `self_consistency`, and which
  // does hold. Partition invariance is still worth checking over the systems
  // alone, where it is sound and where it caught a real bug: settlement timers
  // seeded to zero on every retail map.
  //
  // `Balcans` is the scenario that shows it, because it is the wildlife map.
  // `Crossroads` passes at 400 turns and `Numantia` at 40 -- passing here is
  // luck about which scripts reach a `rand`, not evidence.
  options.partition_invariance = !args.scripts;
  if (args.scripts) {
    std::printf(
        "note      partition invariance not checked: what a waking script reads depends\n"
        "          on the partition, and shipped scripts draw from the world RNG when they wake.\n"
        "          Re-run with --no-scripts to check it over the systems alone.\n\n");
  }

  std::unique_ptr<conformance::TraceOracle> oracle;
  if (!args.golden.empty() && !args.record_golden) {
    const std::vector<std::byte> bytes = read_file(args.golden);
    if (bytes.empty()) {
      std::fprintf(stderr, "cannot read golden file %s\n", args.golden.c_str());
      return 1;
    }
    auto parsed = conformance::read_trace(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!parsed.ok()) {
      std::fprintf(stderr, "golden file %s did not parse (error %d)\n", args.golden.c_str(),
                   static_cast<int>(parsed.error()));
      return 1;
    }
    oracle = std::make_unique<conformance::TraceOracle>(std::move(parsed.value()), args.golden);
    options.oracle = oracle.get();
  }

  const conformance::Report report = conformance::run(scenario, args.seed, schedule, options);
  const int status = print_report(report);

  if (args.record_golden && !args.golden.empty()) {
    const auto text = conformance::write_trace(report.trace);
    if (!text.ok()) {
      std::fprintf(stderr, "\nrefusing to write a golden file: a reserved channel is non-zero\n");
      return 1;
    }
    if (!write_text(args.golden, text.value())) {
      std::fprintf(stderr, "\ncannot write %s\n", args.golden.c_str());
      return 1;
    }
    std::printf("\nwrote %s (%zu turns, %zu bytes)\n", args.golden.c_str(),
                report.trace.entries.size(), text.value().size());
  }
  return status;
}

/// `imconform buttons <game> <map> <class> [health]`: the command bar a player
/// sees with one object selected -- the first the map places whose class is
/// `<class>` or an heir of it and that a player owns -- at the health the map
/// gave it, or at `health`. One line per button, `lit` or `unlit` as the bar
/// draws it, and nothing advances: the bar is described at turn 0, as
/// `observe` describes it before its first turn. A health set here is the
/// world's alone, so it would not outlive a turn -- combat holds the running
/// figure -- which is why nothing is run after it.
///
/// `--press NAME` (repeatable) then presses that button and runs one turn so
/// that what it queued starts; after each, the object's queue is printed,
/// running command first, each entry after `" | "`. Playtest #14's question
/// -- does a second train press queue behind the first or replace it -- is
/// answered on a shipped barracks this way. (The turns run, so a `health`
/// given as well does not hold past the first press.)
///
/// `NAME` may carry the keys held and a target: `shift+move@500,600` is a
/// Shift press of `move` aimed at that point, `ctrl+trainIArcher` a Ctrl
/// press. Shift is the append and Ctrl the `bModifier`, as the bar reads
/// them (0x005e39f0); the line printed names the row as given.
///
/// `--cancel N` (repeatable, in order with the presses) clicks cell `N` of
/// the info bar's training queue: a `cancel_command` order for the command
/// that cell shows, applied through the stream as every peer would, then a
/// turn. After every step the object's settlement's gold is printed too, so
/// a refund shows.
///
/// `--select N` selects the N objects of `<class>` nearest the first, the
/// first included, among those its owner owns, by distance and then by id: a
/// group command such as `build_catapult` takes the closest of a selection.
/// `--watch N` runs N more turns after the steps and prints, every turn, each
/// selected object's position and running command, and every siege engine's
/// health, whether it is built, and how many units its own settlement holds:
/// playtest #19's question, whether an engine rises before its crew is
/// inside, read turn by turn on a shipped map.
struct ButtonStep {
  bool cancel = false;
  std::string arg;
};

int run_buttons(const MapArgs& args, const std::string& class_name, const char* health,
                const std::vector<ButtonStep>& steps, std::size_t select, std::size_t watch) {
  using namespace imperivm::core::sim;
  Installation install;
  if (!load_installation(args.game, args.map, install)) return 1;
  MapScenario scenario(install, args.map, args.scripts, /*with_commands=*/true);
  // `--skirmish` builds the session as `imrun` and the app do -- terrain,
  // passability, entities and all -- where the default is the thin session
  // above, which has no art: a building there has no doors.
  Skirmish skirmish;
  if (args.skirmish && !prepare_skirmish(args, scenario, skirmish, 1, 1)) return 1;
  std::unique_ptr<conformance::Run> run = scenario.start(args.seed);
  GameSession* session = run == nullptr ? nullptr : session_of(*run);
  if (session == nullptr) {
    std::fprintf(stderr, "the scenario could not be built\n");
    return 1;
  }
  World& world = session->world();
  const ClassGraph* graph = world.class_graph();
  const ClassIndex base = graph == nullptr ? kNoClass : graph->find(class_name);
  if (base == kNoClass) {
    std::fprintf(stderr, "no class %s\n", class_name.c_str());
    return 1;
  }
  const WorldObject* found = nullptr;
  for (const WorldObject& object : world.objects()) {
    if (object.state.owner != kNoPlayer && world.class_is_a(object.id, base)) {
      found = &object;
      break;
    }
  }
  if (found == nullptr) {
    std::fprintf(stderr, "no player owns a %s on this map\n", class_name.c_str());
    return 1;
  }
  const ObjectId id = found->id;
  const PlayerId owner = found->state.owner;
  if (health != nullptr) (void)world.set_health(id, std::atoi(health));
  const WorldObject* object = world.find(id);
  std::printf("object    %u %.*s player %u health %d/%.*s tier %d\n", static_cast<unsigned>(id),
              static_cast<int>(graph->at(object->class_index).id.size()),
              graph->at(object->class_index).id.data(), static_cast<unsigned>(owner),
              object->state.health,
              static_cast<int>(graph->property(object->class_index, "maxhealth").size()),
              graph->property(object->class_index, "maxhealth").data(),
              object->state.damage_state);
  std::printf("at        %d,%d\n", object->state.position.x, object->state.position.y);
  std::vector<ObjectId> selected{id};
  if (select > 1) {
    const Point from = object->state.position;
    std::vector<std::pair<std::int64_t, ObjectId>> near;
    for (const WorldObject& other : world.objects()) {
      if (other.id == id || other.state.owner != owner || !world.class_is_a(other.id, base)) continue;
      const std::int64_t dx = other.state.position.x - from.x;
      const std::int64_t dy = other.state.position.y - from.y;
      near.emplace_back(dx * dx + dy * dy, other.id);
    }
    std::sort(near.begin(), near.end());
    for (std::size_t i = 0; i < near.size() && selected.size() < select; ++i) {
      selected.push_back(near[i].second);
    }
    std::printf("selected  %zu\n", selected.size());
  }
  session->selections().assign(owner, selected);
  CommandBar bar(*session);
  const std::vector<CommandButton> buttons = bar.describe(owner);
  for (const CommandButton& button : buttons) {
    std::printf("button    %s %s key=%s gold=%d\n", button.name.c_str(),
                button.enabled ? "lit" : "unlit", button.key.c_str(), button.cost_gold);
  }
  std::printf("buttons   %zu\n", buttons.size());

  const auto print_state = [&] {
    const CommandSystem* commands = command_system(world);
    const CommandQueue* queue = commands == nullptr ? nullptr : commands->find(id);
    std::printf("queue     %zu", queue == nullptr ? std::size_t{0} : queue->size());
    if (queue != nullptr) {
      for (const Command& command : queue->entries) {
        std::printf(" | %s", command.name.empty() ? command.verb.c_str() : command.name.c_str());
      }
    }
    std::printf("\n");
    const Settlement* town = nullptr;
    if (EconomySystem* economy = economy_of(world)) {
      const WorldObject* slot = world.find(id);
      if (slot != nullptr && slot->settlement != kNoObject) town = economy->settlements().for_object(slot->settlement);
      if (town == nullptr) town = economy->settlements().for_object(id);
    }
    if (town != nullptr) std::printf("gold      %d\n", town->warehouse.gold);
  };
  InfoBar infobar(*session, {}, {});
  if (!steps.empty()) {
    // The gold before the first step, for a refund to be measured against.
    if (EconomySystem* economy = economy_of(world)) {
      const WorldObject* slot = world.find(id);
      const Settlement* town = slot != nullptr && slot->settlement != kNoObject
                                   ? economy->settlements().for_object(slot->settlement)
                                   : nullptr;
      if (town == nullptr) town = economy->settlements().for_object(id);
      if (town != nullptr) std::printf("gold      %d\n", town->warehouse.gold);
    }
  }
  for (const ButtonStep& step : steps) {
    if (step.cancel) {
      // The strip's click: the cell's command, by its id (0x006bf7c0).
      const std::size_t cell = static_cast<std::size_t>(std::max(0, std::atoi(step.arg.c_str())));
      const SelectionInfo info = infobar.describe(owner);
      NetTurn turn;
      NetOrder order;
      order.kind = NetOrderKind::cancel_command;
      order.issuer = owner;
      order.target.object = id;
      order.command_id = cell < info.queue.size() ? info.queue[cell].command : 0;
      turn.orders.push_back(order);
      const NetTurnReport report = apply_turn(world, turn);
      std::printf("cancel    %zu %s\n", cell, report.cancels == 1 ? "done" : "nothing");
      run->advance(args.length);
      print_state();
      continue;
    }
    const std::string& spec = step.arg;
    // `[ctrl+][shift+]NAME[@X,Y]`.
    CommandBar::Keys keys;
    std::string_view rest = spec;
    for (;;) {
      if (rest.substr(0, 5) == "ctrl+") {
        keys.ctrl = true;
        rest.remove_prefix(5);
      } else if (rest.substr(0, 6) == "shift+") {
        keys.shift = true;
        rest.remove_prefix(6);
      } else {
        break;
      }
    }
    std::string name(rest);
    bool aimed = false;
    OrderTarget target;
    if (const std::size_t at = name.rfind('@'); at != std::string::npos) {
      int x = 0;
      int y = 0;
      if (std::sscanf(name.c_str() + at + 1, "%d,%d", &x, &y) == 2) {
        aimed = true;
        target.point = Point{x, y};
        name.resize(at);
      }
    }
    CommandBar::Press verdict = bar.press(owner, name, keys);
    if (aimed && verdict == CommandBar::Press::waiting) {
      verdict = bar.aim(owner, name, target, keys) ? CommandBar::Press::issued : CommandBar::Press::unknown;
    }
    const char* said = verdict == CommandBar::Press::issued     ? "issued"
                       : verdict == CommandBar::Press::waiting  ? "waiting"
                       : verdict == CommandBar::Press::disabled ? "refused"
                                                                : "unknown";
    std::printf("press     %s %s\n", spec.c_str(), said);
    run->advance(args.length);
    print_state();
  }
  const CommandSystem* commands = command_system(world);
  EconomySystem* economy = economy_of(world);
  for (std::size_t turn = 1; turn <= watch; ++turn) {
    run->advance(args.length);
    for (const ObjectId unit : selected) {
      const WorldObject* slot = world.find(unit);
      if (slot == nullptr) continue;
      const CommandQueue* queue = commands == nullptr ? nullptr : commands->find(unit);
      const Command* running = queue == nullptr ? nullptr : queue->running();
      const char* doing = running == nullptr ? "-"
                          : running->name.empty() ? running->verb.c_str()
                                                  : running->name.c_str();
      std::printf("watch     %zu unit %u at %d,%d %s\n", turn, static_cast<unsigned>(unit),
                  slot->state.position.x, slot->state.position.y, doing);
    }
    for (const WorldObject& slot : world.objects()) {
      if (slot.object == nullptr || !slot.object->is_a(NativeClass::catapult)) continue;
      const Settlement* own = economy == nullptr || slot.settlement == kNoObject
                                  ? nullptr
                                  : economy->settlements().for_object(slot.settlement);
      // The sheet row the view draws, the way `WorldView::cursor_for` picks it.
      std::int32_t row = -1;
      const AnimCursor& cursor = slot.object->anim;
      if (slot.timeline.valid() && cursor.anim_slot != kNoAnim && slot.object->entity != nullptr) {
        row = static_cast<std::int32_t>(slot.timeline.row_of_step(cursor.step));
        const EntityAnim* playing = slot.object->entity->anim(cursor.anim_slot);
        if (slot.build_frame >= 0 && playing != nullptr) {
          row = static_cast<std::int32_t>(build_frame_row(*playing, slot.timeline, slot.build_frame));
        }
      }
      std::printf("watch     %zu engine %u health %d built %d inside %d anchor %u anim %d frame %d"
                  " drawn row %d of %u\n",
                  turn, static_cast<unsigned>(slot.id), slot.state.health,
                  slot.state.flags.built ? 1 : 0, own == nullptr ? -1 : own->holder.count(),
                  static_cast<unsigned>(own == nullptr ? kNoObject : own->anchor), cursor.anim_slot,
                  slot.build_frame, row, slot.timeline.rows());
    }
  }
  return 0;
}

void usage() {
  std::fprintf(stderr,
               "usage: imconform self  [turns] [turn-length]\n"
               "       imconform lockstep <game> <map> [turns] [length]\n"
               "       imconform netplay  <game> <map> [turns] [delay] [net-seed]\n"
               "       imconform netjoin  <game> <map> [turns] [net-seed]\n"
               "       imconform observe  <game> <map> [turns]\n"
               "       imconform buttons  <game> <map> <class> [health] [--press [ctrl+][shift+]NAME[@X,Y]]...\n"
               "                          [--select N] [--watch TURNS]\n"
               "                          [--cancel CELL]...\n"
               "       imconform udp-host <game> <map> [--port P] [--turns N] [--delay D] [--drop M]\n"
               "                          [--peers N] [--silence MS] [--wait-join-at N]\n"
               "                          [--speed-on-join SPEED]\n"
               "       imconform udp-join <game> <map> <host:port> [--drop M] [--leave-after N]\n"
               "                          [--crash-after N] [--silence MS] [--speed-at K:SPEED]\n"
               "       lockstep, netplay, netjoin, observe, udp-host and udp-join also take\n"
               "                          [--skirmish [--setup map|net] [--peers N] [--seats A,B] [--gold N] [--strike T]]\n"
               "       netplay and netjoin also take [--speed PER-MILLE]\n"
               "       imconform trace [turns] [turn-length]\n"
               "       imconform map   <game-dir> <map.bfhp> [options]\n"
               "\n"
               "  self   run every check over the built-in synthetic scenario.\n"
               "         Needs no game data; proves the harness works.\n"
               "  trace  print that scenario's trace in the golden-file format.\n"
               "  map    run every check over a real session built from a shipped map.\n"
               "  lockstep two peers over one synthesised command stream, on a shipped map.\n"
               "  netplay  one peer per player, through the turn negotiator, over a loopback\n"
               "           that delays, reorders and repeats every packet.\n"
               "  netjoin  netplay with a player who leaves and two late joiners who take the\n"
               "           seat from the host's save (this engine's late join).\n"
               "  udp-host / udp-join  the same match between processes, over a real socket;\n"
               "           --drop loses that many datagrams in a thousand on purpose; a\n"
               "           udp-join to a match already running joins it late.\n"
               "  buttons  the command bar with the first player-owned <class> selected, at\n"
               "           the map's health or at [health]: one line per button, lit or unlit.\n"
               "  --skirmish  a skirmish the computer plays, loaded as imrun and the app load it:\n"
               "           the peers are the lowest --peers players that hold a stronghold and\n"
               "           give no orders; the check needs a blow between two players. --setup\n"
               "           net (the default) seats them as a networked match's humans; --setup\n"
               "           map plays the map's own setup, as imrun does. udp-host and udp-join\n"
               "           must both be given it. --gold is the settings screen's starting\n"
               "           gold (2500, 5000 or 10000; -1, the default, the map's own), which\n"
               "           a udp-host's start carries to its joiners. --seats A,B,... names\n"
               "           the peers' players instead, keeping the others the computer's.\n"
               "           --strike TURN: from that turn, the first a seat has a unit a right\n"
               "           click sends to attack an enemy, the seat nearest one right-clicks it\n"
               "           -- a blow that waits on no AI; udp-host and udp-join both need it.\n"
               "\n"
               "map options:\n"
               "  --turns N        turns to run (default 50)\n"
               "  --length N       game-time units per turn (default 800)\n"
               "  --seed N         world seed (default 1)\n"
               "  --golden PATH    compare the run against this trace\n"
               "  --record PATH    write the run's trace here instead of comparing\n"
               "  --no-scripts     run the systems with the script VM switched off\n");
}

}  // namespace

/// Takes `--skirmish`, `--setup map|net`, `--peers N`, `--seats`, `--gold N` and `--strike`
/// out of `argv`, for
/// every subcommand that plays a map between peers; what is left is the
/// subcommand's own arguments, in order. False on a malformed one.
bool take_skirmish_flags(int& argc, char** argv, MapArgs& args) {
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--skirmish") {
      args.skirmish = true;
    } else if (flag == "--setup" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value != "map" && value != "net") {
        std::fprintf(stderr, "--setup is map or net\n");
        return false;
      }
      args.map_setup = value == "map";
    } else if (flag == "--gold" && i + 1 < argc) {
      args.gold = static_cast<std::int32_t>(std::strtol(argv[++i], nullptr, 10));
      if (args.gold != 2500 && args.gold != 5000 && args.gold != 10000 && args.gold != -1) {
        std::fprintf(stderr, "--gold is the settings screen's: 2500, 5000, 10000 or -1\n");
        return false;
      }
    } else if (flag == "--peers" && i + 1 < argc &&
               std::strcmp(argv[1], "udp-host") != 0) {
      args.peers = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (flag == "--strike" && i + 1 < argc) {
      args.strike = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (flag == "--seats" && i + 1 < argc) {
      for (const char* at = argv[++i]; *at != '\0';) {
        char* end = nullptr;
        const unsigned long seat = std::strtoul(at, &end, 10);
        if (end == at || seat >= imperivm::core::sim::kNeutralWildlife) {
          std::fprintf(stderr, "--seats is a list of players, as 0,1,3\n");
          return false;
        }
        args.seats.push_back(static_cast<imperivm::core::PlayerId>(seat));
        at = *end == ',' ? end + 1 : end;
      }
      std::sort(args.seats.begin(), args.seats.end());
    } else if (flag == "--speed" && i + 1 < argc &&
               (std::strcmp(argv[1], "netplay") == 0 || std::strcmp(argv[1], "netjoin") == 0)) {
      args.speed = static_cast<std::int32_t>(std::strtol(argv[++i], nullptr, 10));
    } else {
      argv[kept++] = argv[i];
    }
  }
  argc = kept;
  if ((args.map_setup || args.peers != 0 || args.gold != -1 || !args.seats.empty() ||
       args.strike != 0) &&
      !args.skirmish) {
    std::fprintf(stderr, "--setup, --peers, --seats, --gold and --strike are --skirmish's\n");
    return false;
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  MapArgs flags;
  if (command == "lockstep" || command == "netplay" || command == "netjoin" ||
      command == "observe" || command == "udp-host" || command == "udp-join" ||
      command == "buttons") {
    if (!take_skirmish_flags(argc, argv, flags)) return 2;
  }

  if (command == "self" || command == "trace") {
    const std::size_t turns =
        argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 40;
    const std::int32_t length = argc > 3 ? std::atoi(argv[3]) : 400;
    if (turns == 0 || length <= 0) {
      std::fprintf(stderr, "turns must be positive and turn length must be positive\n");
      return 2;
    }
    return command == "self" ? run_self(turns, length) : print_trace(turns, length);
  }

  if (command == "lockstep") {
    if (argc < 4) {
      usage();
      return 2;
    }
    MapArgs args = flags;
    args.game = argv[2];
    args.map = argv[3];
    if (argc > 4) args.turns = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
    if (argc > 5) args.length = std::atoi(argv[5]);
    return run_lockstep(args);
  }

  if (command == "netplay") {
    if (argc < 4) {
      usage();
      return 2;
    }
    MapArgs args = flags;
    args.game = argv[2];
    args.map = argv[3];
    std::uint32_t delay = imperivm::core::sim::kDefaultInputDelay;
    std::uint32_t net_seed = 1;
    if (argc > 4) args.turns = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
    if (argc > 5) delay = static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 10));
    if (argc > 6) net_seed = static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 10));
    if (args.turns == 0) {
      std::fprintf(stderr, "turns must be positive\n");
      return 2;
    }
    return run_netplay(args, delay, net_seed);
  }

  if (command == "netjoin") {
    if (argc < 4) {
      usage();
      return 2;
    }
    MapArgs args = flags;
    args.game = argv[2];
    args.map = argv[3];
    args.turns = 60;
    std::uint32_t net_seed = 1;
    if (argc > 4) args.turns = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
    if (argc > 5) net_seed = static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 10));
    if (args.turns < 50) {
      std::fprintf(stderr, "netjoin needs 50 turns at least: the second joiner is seated at 36\n");
      return 2;
    }
    return run_netjoin(args, net_seed);
  }

  if (command == "observe") {
    if (argc < 4) {
      usage();
      return 2;
    }
    MapArgs args = flags;
    args.game = argv[2];
    args.map = argv[3];
    args.turns = argc > 4 ? static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10)) : 40;
    if (args.turns == 0) {
      std::fprintf(stderr, "turns must be positive\n");
      return 2;
    }
    return run_observe(args);
  }

  if (command == "buttons") {
    if (argc < 5) {
      usage();
      return 2;
    }
    MapArgs args = flags;
    args.game = argv[2];
    args.map = argv[3];
    const char* health = nullptr;
    std::vector<ButtonStep> presses;
    std::size_t select = 1;
    std::size_t watch = 0;
    for (int i = 5; i < argc; ++i) {
      if (std::string_view(argv[i]) == "--select" && i + 1 < argc) {
        select = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        continue;
      }
      if (std::string_view(argv[i]) == "--watch" && i + 1 < argc) {
        watch = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        continue;
      }
      if (std::string_view(argv[i]) == "--press" && i + 1 < argc) {
        presses.push_back(ButtonStep{false, argv[++i]});
      } else if (std::string_view(argv[i]) == "--cancel" && i + 1 < argc) {
        presses.push_back(ButtonStep{true, argv[++i]});
      } else if (health == nullptr) {
        health = argv[i];
      } else {
        usage();
        return 2;
      }
    }
    return run_buttons(args, argv[4], health, presses, select, watch);
  }

  if (command == "udp-host" || command == "udp-join") {
    const bool hosting = command == "udp-host";
    if (argc < (hosting ? 4 : 5)) {
      usage();
      return 2;
    }
    MapArgs args = flags;
    args.game = argv[2];
    args.map = argv[3];
    args.turns = 20;
    UdpArgs udp;
    int i = 4;
    if (!hosting) udp.host = argv[i++];
    for (; i < argc; ++i) {
      const std::string flag = argv[i];
      if (i + 1 >= argc) {
        std::fprintf(stderr, "option '%s' needs a value\n", flag.c_str());
        return 2;
      }
      const unsigned long value = std::strtoul(argv[++i], nullptr, 10);
      if (flag == "--port") {
        udp.port = static_cast<std::uint16_t>(value);
      } else if (flag == "--turns") {
        args.turns = value;
      } else if (flag == "--delay") {
        udp.delay = static_cast<std::uint32_t>(value);
      } else if (flag == "--drop") {
        udp.drop = static_cast<std::uint32_t>(std::min(value, 999ul));
      } else if (flag == "--seed") {
        args.seed = static_cast<std::uint32_t>(value);
      } else if (flag == "--timeout") {
        udp.timeout_ms = static_cast<std::uint32_t>(value);
      } else if (flag == "--peers") {
        udp.peers = value;
      } else if (flag == "--leave-after") {
        udp.leave_after = value;
      } else if (flag == "--crash-after") {
        udp.crash_after = value;
      } else if (flag == "--silence") {
        udp.silence_ms = static_cast<std::uint32_t>(value);
      } else if (flag == "--wait-join-at") {
        udp.wait_join_at = value;
      } else if (flag == "--speed-on-join") {
        udp.speed_on_join = static_cast<std::int32_t>(value);
      } else if (flag == "--speed-at") {
        // TURN:SPEED -- `value` has read the turn; the speed follows the colon.
        const char* colon = std::strchr(argv[i], ':');
        udp.speed_turn = value;
        udp.speed = colon == nullptr ? 0 : static_cast<std::int32_t>(std::strtol(colon + 1, nullptr, 10));
      } else {
        std::fprintf(stderr, "unknown option '%s'\n", flag.c_str());
        return 2;
      }
    }
    if (args.turns == 0) {
      std::fprintf(stderr, "turns must be positive\n");
      return 2;
    }
#if IMPERIVM_HAVE_NET
    udp.strike = args.strike;
    return hosting ? run_udp_host(args, udp) : run_udp_join(args, udp);
#else
    std::fprintf(stderr, "this build has no sockets\n");
    return 2;
#endif
  }

  if (command == "map") {
    if (argc < 4) {
      usage();
      return 2;
    }
    MapArgs args;
    args.game = argv[2];
    args.map = argv[3];
    for (int i = 4; i < argc; ++i) {
      const std::string flag = argv[i];
      const bool has_value = i + 1 < argc;
      if (flag == "--no-scripts") {
        args.scripts = false;
      } else if (flag == "--turns" && has_value) {
        args.turns = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
      } else if (flag == "--length" && has_value) {
        args.length = std::atoi(argv[++i]);
      } else if (flag == "--seed" && has_value) {
        args.seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
      } else if (flag == "--golden" && has_value) {
        args.golden = argv[++i];
      } else if (flag == "--record" && has_value) {
        args.golden = argv[++i];
        args.record_golden = true;
      } else {
        std::fprintf(stderr, "unknown option '%s'\n", flag.c_str());
        return 2;
      }
    }
    if (args.turns == 0 || args.length <= 0) {
      std::fprintf(stderr, "turns must be positive and turn length must be positive\n");
      return 2;
    }
    return run_map(args);
  }

  std::fprintf(stderr, "unknown command '%s'\n", command.c_str());
  usage();
  return 2;
}
