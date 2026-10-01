#pragma once

/// One running game: a world, the systems on it, the scripts driving them, and
/// the host surface they call through.
///
/// ## Why this exists, and why it is the piece that was missing
///
/// Eleven host domains, six simulation systems, a script VM, a scheduler and a
/// registry all existed and all passed their own tests before anything put them
/// in one process. That is not an oversight in hindsight -- it is the exact
/// condition under which this project's worst bugs lived: a `KillCommand` that
/// destroyed its receiver, one `void*` meaning three different types, a query
/// returning the caller's own objects instead of its enemies'. Every one was
/// invisible to a test that exercised a single domain, and every one was in
/// code counted as implemented.
///
/// So the coverage number -- 257 entry points, 77% of the corpus's call sites
/// -- is a **promise, not a measurement**, until a shipped script runs against
/// a real world. This class is what turns it into one.
///
/// ## What it does not do
///
/// It does not read a file. `engine/core` may not (see
/// `docs/engine/architecture.md`), so every input arrives as bytes or as an
/// already-parsed structure the caller owns, exactly as `World::populate_from_map`
/// and `ClassGraph::validate` already work. The app assembles those.
///
/// It does not own the `HostRegistry` either. The registry is world-invariant --
/// `HostFn` is a plain function pointer and the table is built once -- so an
/// embedder builds one per process with `register_all_hosts` and hands it to
/// every session.
///
/// ## Order is the simulation's definition
///
/// Systems run in registration order and this class fixes that order in one
/// place, `kSystemOrder`, for the same reason `host_setup.cpp` keeps the
/// registration manifest as data: changing it changes results, so it should be
/// a line someone edits deliberately rather than a sequence of calls buried in
/// a constructor.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/mutable_settlement.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

/// The order the systems run in, every turn.
///
/// Movement before combat because a unit's attack is resolved where it stands
/// after moving; the economy and heroes after both because they read the
/// outcome; the feeder immediately after the economy, so that a unit drawing
/// from a warehouse sees this turn's production rather than last turn's;
/// commands last because a command issued this turn takes effect on the next. **This is a reading, not a measurement** -- the dumps record state,
/// not the order it was produced in -- and it is written down here so that a
/// future conformance run has one line to change.
///
/// `areas` is next to last and has no `advance` at all: it carries the map's
/// trigger regions, which are geometry and never change (`sim/area.hpp`). It is
/// a system only because `World::systems()` is how a host function reaches its
/// domain's state.
///
/// `fog` is **last, and after movement for a reason**: it stamps what each
/// object can see from where it stands, so running it before movement would
/// reveal the ground a unit left rather than the ground it reached. It is also
/// the one system whose state is deliberately outside the hash
/// (`sim/fog.hpp`), so its position cannot move a conformance run -- which is
/// exactly why it is safe to put last and nowhere else.
inline constexpr std::string_view kSystemOrder[] = {"movement", "combat", "economy", "feeder",
                                                    "hero",     "env",    "command", "match",
                                                    "ai",       "campaign", "areas",
                                                    "fog"};

/// Fetches `.vs` source by the path a class binding names.
///
/// The core cannot open a file, and a session cannot know in advance which
/// scripts a map needs -- that comes from the classes its objects turn out to
/// use. So compilation is driven by demand, and this is the seam: `ClassGraph`
/// says `UNIT_IDLE.VS`, the embedder produces its bytes.
///
/// Returning an empty span is **not** an error. Four `<behavior script=...>`
/// bindings in the shipped class tree name files that are not in `data.pak` at
/// all, and the original runs anyway.
class ScriptResolver {
 public:
  virtual ~ScriptResolver() = default;
  virtual std::span<const std::byte> source(std::string_view path) = 0;
};

/// Everything a session needs that it cannot derive.
///
/// Byte spans, because the core cannot open a file. All of them may be empty:
/// a session with no `CONST.INI` answers `GetConst` from the economy's own
/// fields alone, and one with no translation table returns every string
/// untranslated. Both are honest degradations, and both are what a conformance
/// run wants.
struct SessionInputs {
  /// `Maps/<n>/map.obj.xml` -- the object list.
  std::span<const std::byte> map_objects;
  /// `Maps/<n>/Terrain.pass.grid` -- the authored obstruction bitmap.
  ///
  /// **Three of the map's six `GRID` layers now reach the simulation**, and
  /// this comment used to say this was the only one that ever would. It was
  /// already wrong when `terrain` was added below and is wrong again with
  /// `height`; the three are separate fields because the three declare
  /// different geometry and none of them can be reinterpreted as another.
  ///
  /// This one declares `cell_size = 16` and `bits_per_cell = 1`; the
  /// terrain-type layer declares 64 and 8 and is four times coarser, and the
  /// height layer declares 32 and 8, so no two can share an index and
  /// `ObstructionGrid::from_grid` refuses anything but this one rather than
  /// reinterpreting it (`sim/path.hpp`, `docs/formats/pass.md`). A set bit is
  /// blocked, and the layer is pre-baked: the editor has already stamped the
  /// footprints of the objects it ships with. It is adopted as shipped at
  /// `create`, and rebuilt over the world at `start_match` when `masks` and
  /// its companions below are given -- see them.
  ///
  /// Empty is legal and means an empty grid, which is what every session did
  /// before this field existed -- and an empty grid is *not* neutral: A* over
  /// one lets a unit walk through walls and across water. A conformance run may
  /// still want it, so it degrades rather than failing.
  std::span<const std::byte> passability;
  /// The sixteen `player<i>.xml` documents, in id order.
  std::span<const std::span<const std::byte>> player_setup;
  /// `DATA/CONST.INI`.
  std::span<const std::byte> constants;

  /// `DATA/FORMATIONS.XML` -- the six formation classes and which is default.
  ///
  /// **Empty is not neutral.** `MovementSystem::formations()` answers null for
  /// every lookup without it, so `place_army` returns on its first check and a
  /// hero's warriors have never formed up in any session this engine has run.
  /// `set_formations` had no caller outside `engine/tests` for as long as the
  /// table has existed, which is the same shape of gap as the four this file
  /// already names.
  std::span<const std::byte> formations;
  /// `DATA/AI/AI.INI` for the default profile.
  std::span<const std::byte> ai_profile;
  /// `CURRENTLANG/TRANSLATION.LOC.XML`.
  std::span<const std::byte> translations;
  /// The container's own tables under `Local/<language>/`, merged into the
  /// one above; a document that is not a `<translationtable>` is skipped.
  std::span<const std::span<const std::byte>> localisations;
  /// `game.xml` -- the victory condition, the season, the start player.
  std::span<const std::byte> game;
  /// `Maps/<n>/map.xml` -- the map's name and its extent.
  ///
  /// **Empty is not neutral, and that is why this exists.** `MatchRules` has
  /// carried `map_name` and `map_size` since the match domain was written, with
  /// a comment saying they come from `map.xml` and the caller fills them in --
  /// and no caller ever did, so `map_size` was zero in every headless session
  /// and in every real one the renderer drove. A zero extent makes `Place`
  /// refuse every position on the map (the rectangle is `0 .. map_size - 1`),
  /// `_PlaceEx` clamp everything into a degenerate one, and `GetMapRect`,
  /// `IntoRect`, `ClampToMap` and `MapSize` answer about a map that is not
  /// there. 220 placement call sites resolved to a body that could not succeed.
  ///
  /// The retail values are 8192, 16384 and 32768, and `size_y` always equals
  /// `size_x`. Still optional: a synthetic session that never places anything
  /// does not need one, and refusing to start without it would break every
  /// conformance fixture.
  std::span<const std::byte> map_properties;
  /// `Maps/<n>/Terrain.terrain.grid` -- the terrain-type layer.
  ///
  /// 64 units per cell and one byte per cell, and **the simulation reads it for
  /// exactly one question**: `IsPointInWater`, which asks whether a cell's type
  /// is 13. That is 127 hits in a corpus pass across five idle scripts -- the
  /// lion's, the wolf's, the bear's, the fish's and a ship's unboarding verify
  /// -- and without the layer they all answer "not water" and walk into the
  /// sea.
  ///
  /// Empty is legal and means nothing is water, which is what every session did
  /// before this field existed. It is the *fourth* map payload and each of the
  /// first three arrived the same way: something in the simulation turned out to
  /// be reading a number nobody had ever handed it.
  std::span<const std::byte> terrain;
  /// `Maps/<n>/Terrain.height.grid` -- the elevation layer.
  ///
  /// 32 units per cell and one byte per cell, and the simulation reads it for
  /// exactly one question too: `GetTerrainHeight`, which the bird scripts call
  /// nine times to decide how high to fly and where the ground is. Without it
  /// every point on the map is at sea level, which is what a crow saw before
  /// this field existed -- and a crow that thinks the ground is at zero never
  /// climbs over a hill and lands inside one.
  ///
  /// Empty is legal and means a flat map. The *fifth* map payload, and the
  /// same story as the fourth: something in the simulation turned out to be
  /// reading a number nobody had ever handed it.
  std::span<const std::byte> height;
  /// `Maps/<n>/Terrain.decor.grid`, `DATA\TERRAINS.XML` and
  /// `MAPOBJECTS\DECORS\DECORS.INI`, and the masks: what the passability
  /// rebuild at match start needs beside the layers above.
  ///
  /// The original does not play on the layer the map ships. Once the
  /// `Mutable` placeholders have become a race's towns it rebuilds the whole
  /// layer from the terrain's rules, every object's mask through the
  /// projection and every decoration's (0x00552e95; the rebuild itself is
  /// `core/world/editor.hpp`'s, proven against every shipped layer), so the
  /// grid a match starts on carries the templates' footprints -- and a unit
  /// on Crossroads walks *around* the stronghold its opponent was given
  /// rather than through it, which is what every session here did for as
  /// long as these fields did not exist. All four are optional together:
  /// with any of them missing the layer is adopted as shipped, which a
  /// synthetic session and a conformance fixture want.
  std::span<const std::byte> decor;
  std::span<const std::byte> terrain_table;
  std::span<const std::byte> decor_table;
  PassMaskResolver* masks = nullptr;
  /// `Packs/RandomMapSettlements.bfhp`'s `Maps/1/map.obj.xml` -- the 64
  /// settlement templates a `Mutable` stronghold or village is made into once
  /// the players' races are known. Read by `start_match`, after the races are
  /// drawn; see `sim/mutable_settlement.hpp`. Empty leaves every placeholder
  /// as the map placed it: one building, `race="Mutable"`, no barracks and
  /// nothing for an AI to build with -- which is what every skirmish map ran
  /// with for as long as this field did not exist. Optional, because a
  /// synthetic session has no placeholders and a conformance fixture no pack.
  std::span<const std::byte> settlement_templates;
  /// The same pack's `Maps/1/Terrain.terrain.grid` -- the ground under each
  /// template, 15 (`Invalid`) everywhere a template paints nothing. Read by
  /// `start_match` too: a template lays its ground where it lands
  /// (`stamp_template_ground`), the terrain copied, the height under it to
  /// sea level, the decorations bulldozed, the shore taken out, and the
  /// layers the session plays on -- and the app draws -- change there.
  /// Optional like the templates; without it the towns land on whatever
  /// ground the map had, which is what every session did before.
  std::span<const std::byte> settlement_template_terrain;
  /// `territories.xml`, from a conquest container's root. Optional: only the
  /// one shipped conquest has one, and without it `CampaignSystem` stays empty
  /// and `ConquestBonus` answers the empty string, which is what an adventure
  /// or a skirmish should see.
  /// The container's root `Notes.xml` and the map's own `Maps/<n>/Notes.xml`.
  ///
  /// Two documents, because the original searches two catalogues (`sim/note.hpp`).
  /// **The map's is added first**, so that it wins a duplicate id; nothing in
  /// the shipped corpus can tell, because no id is declared in both and all 122
  /// declarations are in a map's document. Both may be empty, and 36 of the
  /// installation's 50 note documents are.
  std::span<const std::byte> map_notes;
  std::span<const std::byte> notes;

  /// Every `.conv.xml` the container declares for this map, in path order.
  ///
  /// Not two documents the way the notes are: a map keeps one file per
  /// conversation, and the tutorial keeps 23. Empty is legal and means a
  /// mission that never talks, which six of the twenty-four containers are.
  /// Borrowed; must outlive the call that builds the session.
  std::span<const std::span<const std::byte>> conversations;

  std::span<const std::byte> conquest;

  /// `DATA/ITEMS.XML`, the 42 items. Load-time data the hero system's item
  /// store resolves its instances against; empty means no catalogue, which
  /// is what every session had until the info bar needed an item's icon --
  /// the catalogue was parsed in tests and by nothing that ran a map.
  /// Borrowed; must outlive the call that builds the session.
  std::span<const std::byte> items;
  /// The container's own `itemsCustom.xml`, loaded after `items`: the
  /// original keeps two catalogues and mints an item from whichever knows
  /// the id (0x00535640 asks one and then the other); which it asks first
  /// was not read, so a custom item that repeats a shipped id resolves to
  /// the shipped definition here. A hero's `slot0="Ash of druid heart"` in
  /// the Spain campaign was unmintable for as long as this was not read.
  std::span<const std::byte> custom_items;

  /// Every `DATA/COMMANDS/*.XML`, in sorted name order.
  ///
  /// The command table is what `<defaultcmd>` rows name and what a right click
  /// resolves through, so a session without it answers every order "nothing to
  /// do" -- 242 selected units on Numantia, 242 unresolved, no error anywhere.
  /// Sorted because merge order decides which of two rows with one name wins.
  std::span<const std::span<const std::byte>> commands;

  /// The class graph, already parsed and validated. Borrowed; must outlive the
  /// session.
  const ClassGraph* classes = nullptr;
  /// Resolves an entity path to a loaded entity, or null for none.
  EntityResolver* entities = nullptr;
  /// Resolves a `.vs` path to its source, or null. Null means no script runs,
  /// which is a legitimate configuration: it isolates the systems from the VM
  /// so a conformance run can measure one without the other.
  ScriptResolver* scripts = nullptr;
};

/// What the templates' ground did to the map's layers at match start (or
/// at a load, which lays it again). See `SessionInputs::settlement_template_terrain`.
struct GroundReport {
  /// The rectangles changed, in world units: one per template laid, the
  /// box grown by the shore's margin. What a renderer holding its own copy
  /// of the layers has to take again.
  std::vector<edit::WorldRect> changed;
  std::size_t cells_copied = 0;
  std::size_t cells_levelled = 0;
  std::size_t decorations_bulldozed = 0;
  std::size_t shore_cells = 0;
  std::size_t cells_sloped = 0;
};

/// What the passability rebuild at match start did. See
/// `SessionInputs::masks`.
struct PassabilityReport {
  /// Whether the layer was rebuilt at all: every input was there.
  bool rebuilt = false;
  /// Cells that differ from the layer the map shipped -- the templates'
  /// footprints, on a skirmish map; zero on an authored one, since the
  /// rebuild reproduces the shipped layer exactly.
  std::size_t cells_changed = 0;
};

/// What a session did when it was asked to run.
///
/// Traps are collected rather than thrown: a script reaching an unimplemented
/// entry point is the **expected** outcome at 77% coverage, and one trap must
/// not stop the other 576 scripts from telling us what they need. The report is
/// the measurement this class exists to produce.
struct SessionReport {
  std::uint64_t turns = 0;
  std::size_t scripts_started = 0;
  std::size_t scripts_finished = 0;
  std::size_t scripts_running = 0;

  /// One distinct trap, with how many scripts hit it. Sorted by count
  /// descending then by message, so two runs of the same session produce
  /// byte-identical reports -- which is what makes this diffable.
  struct Trap {
    std::string message;
    std::size_t scripts = 0;
    /// The first script id that hit it, for a debugger to attach to.
    script::ScriptId first = script::kNoScript;
  };
  std::vector<Trap> traps;

  /// Commands that resolved to a `.vs` file nothing could compile, and how many
  /// times, ascending by verb.
  ///
  /// **A different failure from a trap and it used to have no channel at all.**
  /// A trap is a script that ran and stopped; this is a script that never
  /// started, and `CommandSystem::service` retires the command silently -- so a
  /// unit that cannot move looks exactly like a unit nobody ordered. It is
  /// beside the traps because it is the same question: what did this map ask
  /// for that this build could not do?
  struct LaunchFailure {
    std::string verb;
    std::size_t commands = 0;
  };
  std::vector<LaunchFailure> launch_failures;

  /// The world hash after the last turn. Two runs from one seed must agree.
  std::uint64_t hash = 0;
};

/// What a `GameSession::load` restored, and what it did not.
///
/// The `hashes` field is the one that matters. `LoadReport` can only report the
/// sections it did not find; this reports whether the ones it *did* find added
/// up to the game that was saved.
struct SessionLoadReport {
  /// What the save says about itself: version, map, turn, hashes, run order.
  SaveMeta meta;
  /// Sections in the file this load did not consume. Empty on a save written
  /// by this build; non-empty means a newer build wrote a section this one has
  /// nowhere to put.
  std::vector<std::string> unconsumed;
  /// Registered systems the save had no section for. Empty on a save written
  /// by this build.
  std::vector<std::string> unrestored_systems;
  std::size_t objects = 0;
  std::size_t scripts = 0;
  /// `sim::verify_hashes` after every section was applied. `channel` names the
  /// actionable half on a mismatch.
  HashMismatch hashes;
};

/// A world, its systems, its scripts, and the seam between them.
class GameSession {
 public:
  /// Build a session. `registry` must outlive it and is not modified.
  ///
  /// Fails only on inputs that cannot be parsed at all. A missing optional
  /// input degrades rather than failing, because a conformance run needs to be
  /// able to leave one out on purpose.
  [[nodiscard]] static Result<std::unique_ptr<GameSession>> create(
      const script::HostRegistry& registry, const SessionInputs& inputs, std::uint32_t seed);

  ~GameSession();
  GameSession(const GameSession&) = delete;
  GameSession& operator=(const GameSession&) = delete;

  [[nodiscard]] World& world() noexcept;
  [[nodiscard]] script::Scheduler& scheduler() noexcept;

  /// Where `pr(...)` goes. Null by default, which makes it a no-op.
  void set_debug_sink(DebugSink* sink) noexcept;

  /// The `HostContext` every host call receives as `CallContext::user`.
  ///
  /// Exposed because anything running a script outside the scheduler needs
  /// *this* one -- `ScriptOrderVerifier` does, to run a `verify=` script -- and
  /// a context rebuilt by the caller would drift on `object_type`,
  /// `translations` or `selections` the first time one of them changed.
  [[nodiscard]] HostContext& host_context() noexcept;

  /// The sixteen per-player selections. See `sim/orders.hpp`.
  ///
  /// Pruned of dead ids once per turn by `advance`, so a selection never holds
  /// a handle to something that has been despawned.
  [[nodiscard]] SelectionTable& selections() noexcept;

  /// Whose screen this is; `kNoPlayer` headless. Not world state -- see
  /// `HostContext::local_player` for why it must not be.
  void set_local_player(PlayerId id) noexcept;

  /// Resolve the skirmish and start its victory-condition scripts.
  ///
  /// Separate from `create` because the setup draws from `World::rng()` and is
  /// therefore part of the simulation's starting state: whether it happens
  /// before or after the object scripts start is an ordering decision that
  /// changes results, so the caller makes it rather than the constructor.
  /// Returns the number of victory scripts started; zero is normal, since 19
  /// of the 22 shipped containers declare no condition.
  ///
  /// **Also where the `Mutable` settlements become a race's.** Once the races
  /// are drawn, every `MutableStronghold` and `MutableVillage` is replaced
  /// from `SessionInputs::settlement_templates`, and the LSA partition and the
  /// GAIKA table are rebuilt over the result. `materialised()` says what
  /// happened. This is the order the original keeps: the lobby decides the
  /// races, the map loads, the placeholders are converted (0x0052658f), and
  /// only then does anything run.
  std::size_t start_match(const MatchOptions& options);

  /// What the last `start_match` made of the map's placeholders.
  [[nodiscard]] const MaterialiseReport& materialised() const noexcept;
  /// What the last `start_match` (or `load`) made of the passability layer.
  [[nodiscard]] const PassabilityReport& passability() const noexcept;
  /// What the templates' ground did to the layers, and where.
  [[nodiscard]] const GroundReport& ground() const noexcept;
  /// The decoration layer as the session holds it -- the map's, less what
  /// the templates bulldozed. Empty when the map has none.
  [[nodiscard]] const Grid& decor() const noexcept;
  /// What `populate_from_map` made of the map: counts, and the object each
  /// authored `<scriptobj>` became (`object_ids`), for an editor's join back
  /// to the list it will write.
  [[nodiscard]] const World::PopulateReport& populated() const noexcept;

  /// Is the match over, and who won. See `sim/match.hpp`.
  [[nodiscard]] MatchStatus match_status() const;
  /// From `viewer`'s seat; see `sim::match_status(world, viewer)`.
  [[nodiscard]] MatchStatus match_status(PlayerId viewer) const;

  /// `EndGame(player, lost)` from outside any script: what a victory sequence
  /// does, done by the embedder. Exists for `imrun --declare-won`, which
  /// needs the mission boundary reachable headless, and for nothing in the
  /// game itself -- the game's matches end from scripts.
  void declare_match(PlayerId player, bool lost);

  /// Start the AI for every computer-controlled player.
  ///
  /// Also separate, and for the same reason: a script id is world state, so
  /// whether the AI's roots come before or after the map's own object scripts
  /// is an ordering decision. Returns the number of players whose AI started.
  std::size_t start_ai();

  /// Start the bonus scripts: for every player whose `PlayerSetup::bonus` is
  /// not -1, the script in `DATA/BonusScripts/` whose file name's leading
  /// number is that bonus -- `001 WEALTH.VS`, `002 RICHES.VS`, `003
  /// HERO.VS`, the three the setup screen offers -- spawned as `void,
  /// Settlement set, int playerid` with the player's first stronghold.
  /// `gbr.exe` (0x00566ed0) lists the directory and keys each file by its
  /// name; `scripts` are the file names it holds. Returns how many started.
  /// **Reading, labelled:** which settlement is the player's, the file lists
  /// nothing but the id; the first stronghold in table order is the one a
  /// skirmish map gives a player.
  std::size_t start_bonuses(std::span<const std::string> scripts);

  /// Start each object's own `idle` script, the one its class binds.
  ///
  /// This is the step that makes a loaded map a running game, and the reason
  /// the class graph is an input: `ClassGraph::resolved_methods` carries the
  /// `<method sig="idle">` binding, inherited, for every class that has one.
  /// Returns how many were started.
  std::size_t start_object_scripts();

  /// The first object id `start_object_scripts` has not considered yet.
  ///
  /// Object ids are never reused, so this one number is the exact record of
  /// which objects have been offered their class's `idle` script. It is world
  /// state in every sense but the hash -- two peers with different watermarks
  /// start different scripts on their next turn -- and it is the one thing a
  /// save carries that belongs to neither the world nor a system, which is why
  /// it has its own section (`kSessionSection`). Exposed so a test can check
  /// that a load put it back rather than left it where `start_object_scripts`
  /// on the fresh session had set it.
  [[nodiscard]] ObjectId script_watermark() const noexcept;

  /// Start the map's `sequences.xml`, which is where the campaign layer lives.
  ///
  /// Every sequence in `manifest` is compiled, so a script that does not parse
  /// shows up in the report; only the autorun-allowed ones are spawned, because
  /// the rest wait for a `RunSequence`. `base` is the directory the manifest's
  /// script root stands for -- `""` for a container-root manifest, `Maps/<n>`
  /// for a map's.
  std::size_t start_sequences(std::span<const SequenceRef> manifest, std::string_view base);

  /// Advance by `turns`, running every system and then the scheduler.
  ///
  /// `turn_length` is game-time milliseconds. **Not a constant**: the original
  /// renegotiates it and lengths of 200, 400, 799 and 800 all occur, so a
  /// caller that wants to reproduce a recorded game feeds the recorded
  /// sequence rather than a fixed number.
  void advance(std::uint64_t turns, std::int32_t turn_length);

  /// What has happened so far.
  [[nodiscard]] SessionReport report() const;

  // -- the campaign between missions ---------------------------------------
  //
  // `sim/campaign.hpp`'s `CampaignCarry`: what one mission leaves for the
  // next. The two calls sit on the session so that an embedder need not reach
  // for the campaign system by name.

  /// Install the carry of an earlier mission over the conquest `create`
  /// configured. **Before `start_sequences`**: the conquest's root sequence
  /// reads `ConquestBonus()` on its first pass, and a carry installed after
  /// it has read is a reward that never runs. Refuses what
  /// `CampaignSystem::restore` refuses, and a session with no campaign
  /// system at all.
  [[nodiscard]] Status restore_campaign(const CampaignCarry& carry);

  /// What this session leaves for the next mission, given how it ended:
  /// `CampaignSystem::carry`. Empty territories when there is no campaign.
  [[nodiscard]] CampaignCarry campaign_carry(std::string_view container,
                                             const MissionResult& result) const;

  // -- the saved game ----------------------------------------------------
  //
  // Format: docs/formats/save.md. Envelope: `sim/save.hpp`.

  /// The whole session as bytes: the world, the scheduler, the selections and
  /// one section for each of the ten systems.
  ///
  /// **No filesystem.** The core cannot open a file
  /// (docs/engine/architecture.md), so this hands back the bytes and putting
  /// them inside a `.bfhp` container -- which is where the original kept its
  /// saves -- is `engine/gamedata`'s or `engine/app`'s job.
  ///
  /// `map` is the map identity to record, and is what `load`'s own `map` is
  /// checked against. There is no canonical map identity in this codebase yet,
  /// so the spelling is the caller's; empty means "the writer did not say",
  /// which a loader may accept but cannot check.
  [[nodiscard]] Result<std::vector<std::byte>> save(std::string_view map = {}) const;

  /// Put this session back where `bytes` left it.
  ///
  /// **The session must already have been built from the same game data.** A
  /// save carries turn-mutable state, not the class graph, the command table,
  /// the AI profile or the shipped catalogs -- `src/sim/save_systems.cpp` says
  /// why -- so the flow is `create` with the same `SessionInputs`, then `load`.
  /// Nothing else is required: the script library is compiled here from the
  /// restored world, so a session that has not had `start_match`, `start_ai` or
  /// `start_object_scripts` called on it loads correctly, and one that has is
  /// equally fine, because the saved coroutines replace whatever was running
  /// **and the saved watermark replaces whatever the sweep had reached**. The
  /// second half is not decoration: a fresh session's watermark is the map's
  /// `next_id()`, and a load that kept it re-offered `idle` to every object
  /// spawned since the match began, on the first turn after the load. Whether
  /// the per-turn sweep runs at all comes back with it: a saved watermark means
  /// the saved game offered `idle` to what it spawned, whatever this session
  /// had been asked. Until it did, a session loaded without having been
  /// started never gave a unit trained after the load its `idle`, which a late
  /// joiner mid-war found within a few turns.
  ///
  /// Refuses, before touching anything, a save whose state-vector version, map
  /// identity or system run order is not this session's. Refuses afterwards, as
  /// `FormatError::malformed`, a save whose recorded hashes do not match the
  /// restored world -- the check that turns "did every section come back?" from
  /// an audit into a runtime answer. `report` carries the detail either way.
  ///
  /// **On any failure after the world section has decoded, this session is no
  /// longer usable** and the caller must discard it. That is deliberate: a
  /// half-restored session that kept running would be the silent divergence the
  /// whole format exists to prevent.
  [[nodiscard]] Status load(std::span<const std::byte> bytes, std::string_view map = {},
                            SessionLoadReport* report = nullptr);

 private:
  GameSession();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace imperivm::core::sim
