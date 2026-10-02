#pragma once

// The command queue: how every `.vs` script tells an object to do anything, and
// the four cross-domain orders that compose movement with combat and heroes.
//
// Contract: sim/system.hpp. Movement: sim/movement.hpp. Class methods:
// game/class_graph.hpp. Command metadata: `DATA\COMMANDS\*.XML`.
// Inventory: docs/formats/vs-host-api.md; verb table: docs/formats/sc-xml.md.
//
// `AddCommand` (215 call sites) and `SetCommand` (200) are the first and third
// most used members in the whole host API after the trivial accessors, and
// between them they are the *only* way a script starts a behaviour. Everything
// else -- `Goto`, `Attack`, `Idle` -- is what a command's own script does once
// it is running.
//
// ## What a command is
//
// A verb, an optional argument, and a `.vs` script.
//
// `DATA\CLASSES\*.SC.XML` binds 187 verbs to scripts: `<method sig="engage"
// vs="data/subai/unit_engage.vs"/>` on `Unit`, inherited down the class tree.
// `AddCommand(true, "engage")` names that row. Running the command *is* running
// the script, with the receiver as its first argument and the command's
// argument as its second -- which is exactly what the scripts' own headers
// declare: `UNIT_MOVE.VS` opens `// void, Obj This, point pt`.
//
// A command therefore ends when its script returns. Nothing else can decide it,
// which is why this is a `System`: the scheduler knows a coroutine finished and
// has no way to tell the queue, so the queue asks, once per turn, in ascending
// object id.
//
// ## The queue, read off the corpus
//
// **`command(i)` indexes the queue, and `command()` is `command(0)`.**
// `WALL_PATROL.VS` builds a three-deep queue and then walks it:
//
//     s1.SetCommand("goto", A);
//     s1.AddCommand(false, "goto", B);
//     s1.AddCommand(false, "guard", This);
//     ...
//     i = 0;
//     while (s1.command(i) == "goto") i += 1;
//     if (s1.command(i) != "guard") { ...rebuild... }
//
// so index 0 is the running command, `command()` returns its **name** as a
// string (122 of the 137 sites are `==`/`!=` against a literal), and the
// running command is *in* the queue rather than beside it.
//
// **`SetCommand` replaces the whole queue and aborts what is running.**
// `AI HELPERS\GUARD.VS` proves both halves in one function, which is the
// cleanest controlled comparison in the corpus:
//
//     if (o.command == "idle" || o.command == "move") o.SetCommand("move", pt);
//     if (o.command == "engage" && n > 2) { o.AddCommand(true, "move", pt);
//                                          o.KillCommand(); }
//
// `SetCommand` needs no `KillCommand` and `AddCommand` does. It has to abort:
// `idle` is `UNIT_IDLE.VS`, a `while(1)` that never returns, so a `SetCommand`
// that waited for it would never take effect.
//
// **`AddCommand(front, verb, arg)` inserts; it never disturbs what is running.**
// `front == true` inserts immediately *behind* the running command (index 1),
// `false` appends. The `AddCommand(true, ...); KillCommand();` idiom -- 5 sites
// -- only makes sense that way round: if `true` displaced the runner there
// would be nothing left for `KillCommand` to end but the command just added.
//
// The ordering follows from `HERO_AI_KILLALL.VS`, which is running `ai_killall`
// when it says
//
//     .AddCommand(true, "ai_killall");
//     .AddCommand(true, "advance", set.GetCentralBuilding.pos);
//     break;
//
// and ends with the comment `// else commands "ai_killall" / "advance" cycle`.
// Inserting each at index 1 leaves `[ai_killall(running), advance, ai_killall]`;
// the script then returns, the runner retires, and the unit advances and *then*
// resumes hunting -- which is what the comment says happens. Appending would
// give the reverse order and the comment would be false. `UNIT_ADVANCE.VS`
// (`advance` then `engage`, to engage now and resume advancing) and
// `UNIT_PROTECT.VS` (`protect` then `engage`) say the same thing twice more.
//
// **`ClearCommands` clears the pending tail and leaves the runner.**
// `CATAPULT_DISBAND.VS`: `ClearCommands(); AddCommand(false, "move", pt);
// KillCommand();` -- append then kill only reaches the appended command if the
// runner survived the clear.
//
// **`KillCommand` ends the running command, not the object.** 43 sites, and the
// giveaway is `ESH_FOODTRADE.VS`: `wagon.AddCommand(false, "unload", hall);
// wagon.KillCommand();`. It also runs on `ObjList`, `Hero` and `Building`.
// Killing the *object* there would destroy the mule that was just loaded.
//
// **A queue is never empty.** `GUARD.VS` tests `o.command == "idle"` as an
// ordinary resting state and every unit class binds `<method sig="idle">`, so
// when the last command retires the class's default verb is enqueued again.
// `UNIT_STANDSTILL.VS`'s `if (.CmdCount != 1)` reads the same way: one command
// is the resting count, not zero.
//
// ## What is deliberately not here
//
//   * **`verify`.** `<method sig="attack" verify="..."/>` binds a predicate
//     script. Real and not implemented: a verifier has to be able to *refuse*
//     a command, and nothing in the corpus shows what a refused `AddCommand`
//     does to the caller. Wiring one guess in would put behaviour in front of
//     the evidence. (`onfinish` *is* here -- see `finish_command` -- because the
//     research ledger is written by nothing else: `RESEARCH.VS` marks an
//     upgrade `researching` and `ONFINISH_RESEARCH.VS` marks it `researched`,
//     and without the second every upgrade any player ever started stayed
//     in progress for the rest of the match.)
//   * **The ambient command globals.** `cmdparam`, `cmdcost_gold`,
//     `cmdcost_food`, `cmdcost_pop`, `cmdcost_stamina` and `cmdwaiting` are
//     bound by the engine while a command's script runs. They are read through
//     `script::Host::global`, which the object model owns, so this file exposes
//     `CommandSystem::command_of_script` and stops there.
//   * **The squad orders, `SetCmd` and `AIDest`.** They were blocked here on
//     representation: no `script::Value` could name a squad, and `AIDest`
//     returns a `GAIKA`, which existed nowhere. Both now have one --
//     `sim/squad.hpp` packs a squad's `(index, player)` key into a handle and
//     `sim/gaika.hpp` establishes that a GAIKA is an integer index -- and the
//     entry points live in `sim/squad.hpp`'s own slice, `register_squad_host`,
//     because that is where the squad table's owner is. `SetCmd` composes with
//     `CommandSystem::set_command`, once per member of the squad.
//
// ## Nothing here is hashed
//
// `scriptstate` is zero in all nine of the original's desync dumps: the shipped
// build kept per-object script state out of the determinism contract, and a
// command queue is a queue of scripts. The one piece of it that *is* world state
// is the id counter, and that already lives on `World` -- `next_command_id()`,
// which reproduces the dumps' `cmdidseed`. So `CommandSystem` overrides
// `hash` with nothing, on purpose. Adding the queue to the hash would pull a
// subsystem into the contract that the original left out.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::script {
class Scheduler;
}

namespace imperivm::core::sim {

/// Where a command script gets compiled; defined in `sim/host_context.hpp`.
class ScriptLibrary;

// --------------------------------------------------------------------------
// the command table -- DATA\COMMANDS\*.XML
// --------------------------------------------------------------------------
//
// A second, *player-facing* name space on top of the class methods: 399
// distinct `<cmd name="...">` rows over 35 files, each carrying the button, the
// hotkey, the costs, and a `method=` naming the `<method sig>` that actually
// runs. `GetCmdCost`, `GetCmdStaminaCost`, `Obj.ExecCmd` and `.cmddelay` all
// index this table; `AddCommand` and `SetCommand` index the method table
// directly and never touch it.
//
// Every field below is an attribute that occurs in the shipped files. The
// attribute census over all 35: `costgold` 265, `costfood` 255, `execdelay`
// 269, `costpop` 88, `coststamina` 26, `priority` 395, `method` 325, `param`
// 286, `immediate` 33. Names are unique across the whole set (399 rows, 399
// distinct names), so one flat table is enough.

/// One `<cmd>`.
struct CommandDef {
  std::string name;
  /// `method=`, or `name` when the row declares none. This is the
  /// `<method sig>` that gets queued.
  std::string method;
  /// `param=`, handed to the script as the ambient `cmdparam`.
  std::string param;

  /// `button=` -- the icon the command bar draws for the row, a path under
  /// `%Buttons%` (`gameres/CmdBar`); `queueicon=` -- the portrait the info
  /// bar's `BuildingQueue` draws for a queued copy of it; `key=` -- its
  /// keyboard shortcut. All three are the interface's and nothing in the
  /// simulation reads them.
  std::string button;
  std::string queue_icon;
  std::string key;
  /// `traincommand="yes"` -- the row trains a unit, which is what the queue
  /// strip shows and the rest of the queue does not.
  ///
  /// **A train row is never inserted replacing.** `gbr.exe` keeps the flag at
  /// `[def+0x1b4]` (the loader compares the attribute with `"no"`, 0x004f3fb1)
  /// and reads it twice on the way to a queue: the command bar clears the
  /// replace flag of a train row's order whatever Shift says (0x005e39f0),
  /// and the per-object issue every order reaches -- the bar, a right click,
  /// `ExecCmd` -- hands the object's insert `replace && !traincommand`
  /// (0x004efbbb). So clicking two units' buttons on a barracks queues both
  /// (playtest #14). 108 shipped rows say yes: every `train`, `trainex` and
  /// `trainpeasant` row, and 32 others -- the arena's `hirehero`, the
  /// blacksmith's schools, the shipyard's `build_ship`, the tavern's `addpop`
  /// and trades, and the catapult's `attack`, `attack_ground`, `autofire`,
  /// `stop` and `disband`, which a right click reaches.
  bool train_command = false;

  std::int32_t cost_gold = 0;
  std::int32_t cost_food = 0;
  std::int32_t cost_pop = 0;
  std::int32_t cost_stamina = 0;
  /// `execdelay=`, in game-time milliseconds. What `.cmddelay` reports.
  std::int32_t exec_delay = 0;
  std::int32_t priority = 0;
  /// `immediate="1"` -- 33 rows. Recorded and **not acted on**: no corpus site
  /// distinguishes an immediate command from an ordinary one.
  bool immediate = false;

  /// `offset="1"` -- 18 rows, and every one of them is a verb a *group* is
  /// given: `move`, `attack`, `advance`, `capture`, `patrol`, `explore`,
  /// `stand_position`, `transport`.
  ///
  /// It is what makes a selection arrive spread out instead of stacked on one
  /// cell: each member keeps its own offset from the group's centroid, which is
  /// exactly what `ObjList.SetCommandOffset` computes for the scripts. The
  /// reader used to drop the attribute, so a player's group order sent every
  /// unit to the identical point and they converged.
  bool offset = false;

  /// `rollover=` -- the command's own name as the tooltip shows it, and a
  /// **translation key** rather than a literal (`docs/formats/interface-ini.md`
  /// makes the same point about `HelpText` and `Rollover` in an `.ini`). 384 of
  /// the 404 shipped rows carry one.
  ///
  /// It is here because the `rollover()` family formats it and the family is
  /// 86 shipped call sites across 26 group verifiers. Nothing in the simulation
  /// reads it: see `sim/feedback.hpp` for the whole argument.
  std::string rollover;
  /// `description=` -- the sentence under the name, and a translation key too.
  /// 253 rows.
  std::string description;

  /// `<src obj="…">` -- the classes that offer this command.
  ///
  /// **This is the class-to-command edge, and it runs from the command's side.**
  /// A `.sc.xml` class lists its `<method sig>` bindings and never its commands;
  /// the commands name the classes. `gbr.exe` builds the reverse index at load
  /// and `Settlement::FindResearchLab` (0x0042d010) walks it: for each building
  /// of the settlement it takes the class at `[obj+0x3c]`, walks the list at
  /// `[class+0x1f8]` and compares each node's row against the command's.
  ///
  /// Matched against the class **tree**, not against the leaf: `<src
  /// obj="Unit"/>` on `move` offers it to every unit class. The other `<src>`
  /// attribute, `sticky`, is not carried -- nothing in the simulation reads it.
  std::vector<std::string> sources;
  /// `<nsrc obj="…">` -- the classes that do *not* offer it, although an
  /// ancestor in `sources` does: `move` is every `Unit`'s but a `Sentry`'s.
  std::vector<std::string> excludes;

  /// `groupverifier=` -- `bool f(ObjList objs, str OUT reasonText)`, run over
  /// the selection to decide whether the row's button is enabled and, when
  /// it is not, why. 26 scripts over 340 rows; `verify_research.vs` alone
  /// guards 153.
  std::string group_verifier;
  /// `groupdispatch=` -- `void f(ObjList objs, point pt, Obj obj, bool
  /// bReplace, bool bModifier, int player)`: a script that issues the order
  /// itself, in place of one command per actor. 18 rows.
  std::string group_dispatch;
  /// `cursor=` -- the cursor while the row waits for its target: `attack`,
  /// `do_something`, `move_in_fight`. Empty for the arrow.
  std::string cursor;
  /// The `<cmdtext target="…">` classes: what the row can be aimed at, `""`
  /// meaning a point on the ground. A row with none is issued the moment its
  /// button is pressed -- `train`, `research`, `stand_position`; a row with
  /// any waits for a click on the map.
  std::vector<std::string> targets;

  [[nodiscard]] bool needs_target() const noexcept { return !targets.empty(); }
};

/// `DATA\COMMANDS\*.XML`, merged.
///
/// Kept sorted by folded name so that lookup is a binary search and iteration
/// order does not depend on which file was read first. A name that arrives
/// twice overwrites, which is the convention `ClassGraph` already uses for
/// duplicate `<method sig>`; it does not happen in the shipped data.
class CommandTable {
 public:
  /// Merge one `<commands>` document. Refuses a document whose root is
  /// something else, so that handing it a class file fails loudly.
  Status merge(std::span<const std::byte> xml);

  /// Case-insensitive: `cmdparam`-built names and script literals disagree on
  /// case, and `Obj.ExecCmd` is called with both.
  [[nodiscard]] const CommandDef* find(std::string_view name) const noexcept;
  [[nodiscard]] std::span<const CommandDef> commands() const noexcept { return commands_; }
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
  [[nodiscard]] bool empty() const noexcept { return commands_.empty(); }

  /// Add or replace one row directly, for a test that does not want to build an
  /// XML document to say `costgold="50"`.
  void set(const CommandDef& def);

 private:
  [[nodiscard]] std::size_t lower_bound(std::string_view folded) const noexcept;

  std::vector<CommandDef> commands_;  ///< sorted by folded name
};

// --------------------------------------------------------------------------
// one queued command
// --------------------------------------------------------------------------

/// What kind of argument a command carries.
///
/// The corpus passes a point (`AddCommand(true, "move", pt)`), an object
/// (`AddCommand(true, "enter", bld)`), or nothing (`AddCommand(true,
/// "engage")`). Those are the three, and the script's own header comment says
/// which it expects: `// void, Obj This, point pt`.
enum class CommandArgKind : std::uint8_t {
  none,
  point,
  object,
};

/// One entry in an object's queue.
struct Command {
  /// From `World::next_command_id()`, the counter the dumps call `cmdidseed`.
  /// Monotone, never reused, so a stale reference reads as gone.
  std::uint32_t id = 0;

  /// The `<method sig>` to run: "move", "engage", "enter". Case is preserved
  /// but comparison is case-insensitive, as everywhere else in the class graph.
  std::string verb;

  CommandArgKind arg_kind = CommandArgKind::none;
  Point point{};
  ObjectId object = kNoObject;

  /// `<cmd param="...">` when the command came in through `ExecCmd`. Empty for
  /// a direct `AddCommand`.
  std::string param;
  /// The `<cmd name>` the command came in as, when it came through a row --
  /// `ExecCmd`, a player's order -- and empty for a direct `AddCommand`. The
  /// info bar's queue strip looks the row up by it for its `queueicon`.
  std::string name;

  std::int32_t cost_gold = 0;
  std::int32_t cost_food = 0;
  std::int32_t cost_pop = 0;
  std::int32_t cost_stamina = 0;
  /// `execdelay`, which `.cmddelay` reports while this command is at the head.
  std::int32_t delay = 0;

  /// `Unit.GetCommanded` / `Unit.SetCommanded` -- the corpus's own comment for
  /// the latter is `/// clear user commanded flag`. Set when the command came
  /// from a player action rather than from a script's own decision.
  bool user = false;

  /// The coroutine running this command, or `kNoScript` before it starts and
  /// after it finishes.
  script::ScriptId script = script::kNoScript;
  /// Whether the launch has been attempted. Distinct from `script != kNoScript`
  /// so that a verb whose class binds no script is retired rather than retried
  /// every turn forever.
  bool started = false;
  /// Game time the command reached the head of the queue.
  GameTime started_at = 0;

  [[nodiscard]] script::Value argument() const noexcept;
};

/// One object's queue. Entry 0 is the running command.
struct CommandQueue {
  std::vector<Command> entries;
  /// The progress bar `Obj::Progress` stamps: game time it started and will
  /// end, or both zero for none. The original writes `[obj+0x128]` and
  /// `[obj+0x12c]`; nothing in the registered surface reads them back, and
  /// for as long as nothing drew them they were not kept here. The info bar's
  /// `BuildingQueue` draws them. **Display, not hashed**: two peers can differ
  /// on it without disagreeing about the game.
  GameTime progress_start = 0;
  GameTime progress_end = 0;

  [[nodiscard]] bool empty() const noexcept { return entries.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
  /// The running command, or null when the queue is empty.
  [[nodiscard]] const Command* running() const noexcept {
    return entries.empty() ? nullptr : &entries.front();
  }
};

/// The verb an object falls back to when its queue drains.
///
/// Every unit, building and animal class in `DATA\CLASSES` binds
/// `<method sig="idle">`, `GUARD.VS` tests `o.command == "idle"` as a resting
/// state, and `UNIT_STANDSTILL.VS` treats a `CmdCount` of one as normal. A
/// class that binds no `idle` simply keeps an empty queue.
inline constexpr std::string_view kDefaultCommandVerb = "idle";

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

/// Advances every object's command queue, once per turn.
///
/// **It needs a per-turn advance and here is why.** A command ends when its
/// `.vs` script returns. The scheduler is what notices a coroutine finished,
/// and it has no channel back to anything -- `RunReport` counts completions and
/// names traps, and a completed script leaves no trace on the object. So
/// somebody has to poll `Scheduler::alive` for the head of every queue, and the
/// turn loop is the one place that runs in a defined order over a defined set.
/// Doing it from a host call instead would mean a queue only advanced while a
/// script happened to look at it, which is exactly the sort of behaviour that
/// differs between two peers.
///
/// Objects are visited in ascending id, which is spawn order. Queues live in
/// one vector sorted by id and are found by binary search: no unordered
/// container, and no dependence on allocation addresses.
class CommandSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "command"; }

  void advance(World& world, const Turn& turn) override;
  // `hash` is deliberately not overridden. See the file header: `scriptstate`
  // is zero in all nine desync dumps, and the id counter that *is* state lives
  // on `World`.

  // -- configuration -----------------------------------------------------

  /// The scheduler that runs command scripts. Not owned.
  ///
  /// **Null is a supported configuration, not a degenerate one.** With no
  /// scheduler a command is queued and marked running and never retires, which
  /// is what a test of queue mechanics wants: `AddCommand`, `KillCommand` and
  /// `command(i)` are then exercised without a single line of script.
  void set_scheduler(script::Scheduler* scheduler) noexcept { scheduler_ = scheduler; }
  [[nodiscard]] script::Scheduler* scheduler() const noexcept { return scheduler_; }

  /// Where a command's `.vs` file gets compiled on first use. Not owned.
  ///
  /// **Without this, a command whose script is not already in the scheduler's
  /// library never launches at all**, and `service` retires it silently -- so
  /// every `move`, `enter` and `engage` on every map is a no-op. Only two sets
  /// of scripts are ever preloaded: the files `AI.INI`'s `[Scripts]` manifest
  /// names, and the per-object `idle` methods `GameSession::start_object_scripts`
  /// compiles. A class `<method>` is named by nothing else, so `move` is
  /// reached by this route or not at all.
  ///
  /// Null is supported and degrades the old way: `find_chunk` alone, and a verb
  /// whose file is not loaded retires. `RunAIHelper` takes the same route for
  /// the same reason; see `sim/host_context.hpp`.
  void set_library(ScriptLibrary* library) noexcept { library_ = library; }
  [[nodiscard]] ScriptLibrary* library() const noexcept { return library_; }

  /// Verbs that resolved to a file nothing could compile, and how many times.
  ///
  /// A launch failure is otherwise invisible: the command is erased and the
  /// queue refills with `idle`, which looks exactly like a unit that was never
  /// ordered. Ordered by verb so two runs report identically.
  [[nodiscard]] const std::map<std::string, std::size_t, std::less<>>& launch_failures()
      const noexcept {
    return launch_failures_;
  }

  void set_table(CommandTable table) { table_ = std::move(table); }
  [[nodiscard]] const CommandTable& table() const noexcept { return table_; }
  [[nodiscard]] CommandTable& mutable_table() noexcept { return table_; }

  /// The verb enqueued when a queue drains. `kDefaultCommandVerb` by default;
  /// empty disables the refill entirely.
  void set_default_verb(std::string_view verb) { default_verb_.assign(verb); }
  [[nodiscard]] std::string_view default_verb() const noexcept { return default_verb_; }

  // -- per-object state --------------------------------------------------

  [[nodiscard]] CommandQueue& queue(ObjectId id);
  [[nodiscard]] const CommandQueue* find(ObjectId id) const noexcept;
  [[nodiscard]] CommandQueue* find(ObjectId id) noexcept;
  /// Drop an object's queue, killing whatever it is running.
  void forget(ObjectId id);
  /// `forget` every queue whose object the world no longer holds. Returns how
  /// many. `reap_departed` calls it once a turn: a corpse that died with a
  /// queued command, or an empty queue, kept its entry and `service` walked it
  /// every turn for the rest of the game.
  std::size_t forget_departed(const World& world);
  /// Ids with a queue, ascending. Iteration order is world state.
  [[nodiscard]] std::size_t tracked() const noexcept { return queues_.size(); }

  // -- orders ------------------------------------------------------------

  /// `SetCommand(verb[, arg])`: abort everything and run this.
  ///
  /// Returns the id of the command created, or 0 when `id` names no object or
  /// its settlement cannot pay the command's cost.
  ///
  /// **Every insert pays first, and refuses what cannot be paid for.** The
  /// three below take the cost when the command is queued, not when it starts,
  /// and refuse it -- nothing taken, nothing queued, the queue as it was --
  /// when the settlement's gold or food is short of it or its population short
  /// of `costpop + MinPopulation` (0x005b1760 calling 0x004df070 on a
  /// building). A refused command has still used its id. So a cancel's refund
  /// (`cancel_command`) is always exactly what was charged. See `charge` in
  /// `src/sim/command.cpp`.
  std::uint32_t set_command(World& world, ObjectId id, std::string_view verb,
                            const Command& prototype);
  /// `AddCommand(front, verb[, arg])`. `front` inserts at index 1 -- behind the
  /// running command, ahead of everything else -- and appends when the queue is
  /// empty. `!front` appends. 0 when refused, as `set_command`.
  std::uint32_t add_command(World& world, ObjectId id, bool front, std::string_view verb,
                            const Command& prototype);
  /// An **order** appended through the order core -- `ExecCmd(..., false)`,
  /// a queued player order, `UpgradeBestBarrack` -- rather than a script's
  /// own `AddCommand`. The one difference: a head that is the default verb
  /// (`idle`) is ended, cancelled and refunded as `KillCommand` would, so the
  /// order runs instead of waiting behind it -- once the order is paid for,
  /// since a refused one (0, as `set_command`) changes nothing.
  ///
  /// **A reading, and labelled as one.** `BARRACK_IDLE.VS` and
  /// `OBJECT_IDLE.VS` are `while (1) Sleep(...)` and never return, so
  /// `ES_STRONGHOLD.VS`'s `barrack.ExecCmd(cmd, pt, obj, false)` -- the only
  /// way a computer player trains an army -- queued behind a running `idle`
  /// and never ran: every computer player on Crossroads raised one unit in
  /// 15,000 turns. The original's orders go through 0x004efc00 to the
  /// per-unit issue at 0x004ef3d0, which serialises the order into the
  /// lockstep stream; the insert on the far side of that was not traced, so
  /// this is what it has to do rather than what it was read to do.
  ///
  /// **Since read, for a row without a `groupdispatch`.** 0x004ef3d0 inserts
  /// through the object's `vtbl+0xb8`, one routine for units and buildings
  /// (0x005b4e90): appending, it pushes the order behind the queue and, when
  /// the running command's name (`[obj+0x10c]`) is `idle`, ends that head
  /// through 0x005b07d0 with index 0, so the order runs. (That routine calls
  /// `vtbl+0x90` first for a command with a cost; the refund is a reading of
  /// that call.) No count is checked there or in the accept test before it
  /// (`vtbl+0x8c`, 0x005b1760): a queue has no limit. `AddCommand` inserts through
  /// `vtbl+0xbc` instead (0x005aff4f) and never looks at the head, which is
  /// the other half of the distinction below.
  ///
  /// It is **not** `AddCommand`'s behaviour, and the corpus says so: eleven
  /// shipped sites -- `ESH_FOODTRADE.VS`, `ES_OUTPOSTSELLGOLD.VS`,
  /// `CATAPULT_DISBAND.VS`, the `CREATE_*_MULE_*.VS` four -- write
  /// `AddCommand(false, verb, arg); KillCommand();` on an object running
  /// `idle`. That idiom only works if the append left the runner alone for
  /// `KillCommand` to end; ending it here would make the kill take the mule's
  /// `unload` instead. `Settlement::Research` is the same failure met from
  /// the other side and fixed with the replace flag it really passes.
  std::uint32_t append_order(World& world, ObjectId id, std::string_view verb,
                             const Command& prototype);
  /// `KillCommand()`: end the running command. Returns false when there was
  /// none. The queue refills with the default verb if it would go empty.
  bool kill_command(World& world, ObjectId id);
  /// `ClearCommands()`: drop the pending tail, leave the runner alone.
  std::size_t clear_commands(World& world, ObjectId id);
  /// `CVXCmdCancelCmd`'s execution (0x004e63c0): the command `command_id` on
  /// `id` is taken out of its queue, wherever it stands. False when there is
  /// no such command -- it finished, or another cancel took it first.
  ///
  /// It finds the command by its id (0x005ae170, a walk of the queue for the
  /// id at `[cmd+0x28]`) and removes it by its index through the routine
  /// `Obj::KillCommand` also ends in (0x005b07d0, which `KillCommand` calls
  /// with 0). That routine first puts the cost back when the row has one --
  /// `costgold`, `costfood` or `costpop` (`[def+0x1e8..0x1f0]`) -- through the
  /// object's `vtbl+0x90` (0x005b18d0), which calls `vtbl+0x88`: on a
  /// building 0x004df400 stores the gold and the food back in the
  /// settlement's warehouse (0x005eec40), takes them off the owner's
  /// spent-gold and spent-food counters, and gives the population back. Then
  /// index 0 is ended as `KillCommand` ends it (its `onfinish` told it was
  /// cancelled), and any other is erased unrun. So a cancelled training is
  /// refunded whether it had started or not, and the rest of the queue keeps
  /// its place.
  ///
  /// **Not modelled, labelled:** the same `vtbl+0x90` runs the row's
  /// `onaddremovescript` with `bAdd` false (0x004e7600, `[def+0x170]`) -- the
  /// twelve `trainex` rows' `TRAINEX_ONADDREMOVE.VS`, which keeps a
  /// `QueuedBuild/<class>` count -- and this engine runs that script on
  /// neither the add nor the remove.
  bool cancel_command(World& world, ObjectId id, std::uint32_t command_id);

  // -- reads -------------------------------------------------------------

  /// `command(index)`. Empty when the index is past the end.
  [[nodiscard]] std::string_view command_name(ObjectId id, std::size_t index = 0) const noexcept;
  /// `CmdCount()`: everything queued, the running command included.
  [[nodiscard]] std::size_t command_count(ObjectId id) const noexcept;
  /// `CmdCount(verb)`: how many queued commands name that verb. Used on
  /// barracks, where each `ExecCmd` appends one training order and the AI
  /// counts them to decide whether to add another.
  [[nodiscard]] std::size_t command_count(ObjectId id, std::string_view verb) const noexcept;
  /// The command a coroutine is running, or null. The hook the ambient
  /// `cmdparam` / `cmdcost_*` globals need; see the file header.
  [[nodiscard]] const Command* command_of_script(script::ScriptId script) const noexcept;

  // -- the commands a script has taken away -------------------------------
  //
  // `Obj::CmdDisable(name)` / `Obj::CmdEnable(name)`, which are one virtual
  // apart in the original (`vtbl+0xdc` and `vtbl+0xe0`) and share the predicate
  // one slot below them.
  //
  // **The state is a per-object set of command names, and it is the second half
  // of a two-part answer.** `vtbl+0xd8` -- the predicate `CmdDisable` consults
  // before it writes, and the one `GetCanExecCmd` calls at 0x00560461 -- is
  // false when `[obj+0x9c]` is zero, which is `ObjectFlags::commands_disabled`,
  // and otherwise walks a linked list at `[obj+0x78]` and is false when the
  // command is in it. So an object may lose *every* command (the flag) or
  // *one* (this list), and the two are asked in that order.

  /// Whether `id` may be offered `name`: not globally silenced, and not
  /// disabled by name. Unknown commands and unknown objects answer false, which
  /// is what `GetCanExecCmd`'s one shipped site already expects of a name the
  /// table does not carry.
  ///
  /// **The building family overrides the virtual**, and this answers for the
  /// override too: every building-family vtable reaches 0x004de580, which
  /// offers a ruin (damage tier 3) only its restoring row -- `method="repair"`,
  /// or `"repopulate"` for an `auto_repair="yes"` class, and not while that is
  /// already running -- and refuses `method="repair"` to a building that is not
  /// a ruin. The command bar asks this of every selected object (0x005e787b);
  /// `building_offers` in `src/sim/command.cpp` has the reading.
  [[nodiscard]] bool command_enabled(const World& world, ObjectId id,
                                     std::string_view name) const noexcept;

  /// `Obj::CmdDisable(name)`. True when this call is what disabled it.
  ///
  /// **Gated on `command_enabled`, and that gate is observable.** The original
  /// tests the predicate first and appends only when it passes, so a
  /// `CmdDisable` issued while `SetCmdEnable(false)` stands writes *nothing* --
  /// and the command comes back enabled when the flag is lifted. A name the
  /// `CommandTable` does not carry is dropped, which is the original's
  /// `jl` past the append when its own name lookup answers a negative index.
  bool disable_command(const World& world, ObjectId id, std::string_view name);

  /// `Obj::CmdEnable(name)`: drop `name` from `id`'s disabled set, however many
  /// copies are in it. True when something was dropped.
  ///
  /// **No shipped script calls it** -- it is absent from the 885-file corpus
  /// and so from `declare_shipped_surface`, and it is deliberately not bound to
  /// a host name here. It exists because it is what makes the state above a
  /// set rather than a one-way latch, and because the original's own pair is
  /// the evidence for what the list holds.
  bool enable_command(ObjectId id, std::string_view name);

  /// `id`'s disabled commands, by canonical table name, ascending. Empty for an
  /// object nothing has taken a command from.
  [[nodiscard]] std::span<const std::string> disabled_commands(ObjectId id) const noexcept;

  /// The `.vs` file bound to `verb` on `id`'s class, or empty.
  ///
  /// Resolved through `ClassGraph::resolved_methods`, cached per class index.
  /// The cache is a pure function of the class graph, so it is not world state
  /// and is not hashed.
  [[nodiscard]] std::string_view script_for(World& world, ObjectId id,
                                            std::string_view verb) const;

  // -- the saved game ----------------------------------------------------
  //
  // Layout and rationale: docs/formats/save.md. The definitions live together
  // in `src/sim/save_systems.cpp` rather than in this domain's own `.cpp`, so
  // that the ten systems' state vectors are one file to audit: adding a field
  // here and forgetting its section shows up as a diff that does not touch the
  // one place every section is written.

  /// Append this system's state to `out`, as one self-describing section with
  /// its own magic and version -- the shape `Scheduler::serialize` established.
  ///
  /// **Written:** every object's queue in id order, and the default verb.
  ///
  /// **This is the section whose absence is silent.** `CommandSystem` has no
  /// `hash` override, so a load that dropped the queues passes
  /// `sim::verify_hashes` unchanged and then diverges a few turns later, when
  /// the objects that were mid-command stop doing anything. That asymmetry is
  /// the reason this pair exists before some that look more important.
  ///
  /// `Command::script` is written as it stands. It names a coroutine in the
  /// scheduler, and the scheduler's own section restores the coroutines under
  /// the same ids, so the two come back consistent -- but only if both sections
  /// are applied. Applying one without the other is not a supported load.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** the `CommandTable` (merged from `DATA/COMMANDS/*.XML` at
  /// load), the scheduler pointer, and `method_cache_`, which is a memo of the
  /// class graph.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  struct Entry {
    ObjectId id = kNoObject;
    CommandQueue queue;
  };
  struct MethodEntry {
    std::string sig;
    std::string script;
    std::string onfinish;  ///< `<method onfinish>`, empty when absent
  };
  struct ClassMethods {
    ClassIndex index = kNoClass;
    std::vector<MethodEntry> methods;  ///< sorted by folded sig
  };

  [[nodiscard]] std::size_t lower_bound(ObjectId id) const noexcept;
  /// Start, retire and refill one object's queue. Bounded: nothing enqueues
  /// during a turn, and the default refill only happens for a verb the class
  /// actually binds.
  void service(World& world, Entry& entry, GameTime now);
  bool launch(World& world, ObjectId id, Command& command, GameTime now);
  /// End the `idle` an object was started with outside its queue, now that a
  /// command holds the object's command slot. See `launch`.
  void end_resting_idle(World& world, ObjectId id);
  void retire(Command& command);
  /// `retire`, then the method's `onfinish` script for a command that had
  /// started, with `canceled` as its second argument. See `finish_command`.
  void retire(World& world, ObjectId id, Command& command, bool canceled);
  /// Run `<method onfinish>` for a command that is over: `(This, bCanceled)`,
  /// spawned on the scheduler with the command kept on `finishing_` so that
  /// `cmdparam` and the costs still answer inside it. Nothing when the class
  /// binds no `onfinish` for the verb.
  void finish_command(World& world, ObjectId id, const Command& command, bool canceled);
  /// The class's `<method>` row for `verb`, or null. Static and private so
  /// the cache's shape stays this file's.
  [[nodiscard]] static const MethodEntry* method_row(const ClassMethods& methods,
                                                     std::string_view verb);
  const ClassMethods& methods_for(const ClassGraph& graph, ClassIndex index) const;

  struct DisabledEntry {
    ObjectId id = kNoObject;
    /// Canonical `CommandDef::name`s, sorted by the folded name.
    ///
    /// The original stores the command's **index** in the global registry. A
    /// name is the stable equivalent here: `CommandTable` is sorted by folded
    /// name, so an index would move the moment a `DATA/COMMANDS/*.XML` merged
    /// in a different order, and the save would then mean something else.
    ///
    /// Sorted rather than appended, which is where the original puts a new
    /// entry. Nothing enumerates the list -- the one reader asks whether a
    /// single name is in it -- so the order is unobservable, and `EnvStore`'s
    /// rule applies to what is unobservable in memory and written down anyway:
    /// iteration order is what gets serialised.
    std::vector<std::string> names;
  };

  [[nodiscard]] std::size_t disabled_lower_bound(ObjectId id) const noexcept;

  std::vector<Entry> queues_;  ///< sorted by id; iteration order is world state
  /// Commands whose `onfinish` script is running, `script` being that script.
  /// Pruned when the script is gone. `command_of_script` reads it after the
  /// queues, so an `onfinish` sees the command it is finishing.
  std::vector<Command> finishing_;
  /// Sorted by id, and only objects a script has taken a command from are in
  /// it: the set is empty on every map at turn zero.
  std::vector<DisabledEntry> disabled_;
  CommandTable table_;
  script::Scheduler* scheduler_ = nullptr;
  ScriptLibrary* library_ = nullptr;
  std::map<std::string, std::size_t, std::less<>> launch_failures_;
  std::string default_verb_{kDefaultCommandVerb};
  /// Sorted by class index. Derived from the class graph, never hashed.
  mutable std::vector<ClassMethods> method_cache_;
};

/// The `CommandSystem` a world is running, or null.
///
/// Found by `System::name()` over `World::systems()`, the same seam
/// `movement_system`, `combat_system_of` and `hero_system_of` use, and for the
/// same reason: a pointer on `HostContext` would have to be added again for
/// every domain and kept correct across a save.
[[nodiscard]] CommandSystem* command_system(World& world) noexcept;

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

/// `Idle()`'s duration when the call gives none.
///
/// **Unknown, and constrained rather than measured.** One site in 577 scripts
/// calls `Idle()` bare, and it sits inside `SHIP_IDLE.VS`'s `while(1)`, so the
/// value has to be positive or that loop spins the scheduler forever. 1000 is
/// the modal explicit argument in the corpus (24 of the 62 sites that pass one).
/// A recording of a ship idling in the retail build would settle it.
inline constexpr std::int64_t kDefaultIdleSlice = 1000;

/// Implement the command slice of the `.vs` host API.
///
/// Returns the number of entry points defined, the convention
/// `register_world_host` and `register_objlist_host` follow, so a caller can
/// assert the count rather than trust it.
///
/// Exactly these, with the arities `docs/formats/vs-host-api.md` records, in
/// descending call frequency:
///
///   members   `AddCommand/2`, `AddCommand/3` (215 sites), `SetCommand/1`,
///             `SetCommand/2` (200), `command/0`, `command/1` (137), `Idle/0`,
///             `Idle/1` (63), `GotoAttack/3`, `GotoAttack/4` (47),
///             `KillCommand/0` (43),
///             `CmdCount/0`, `CmdCount/1` (29), `FormKeepMoving/1` (29),
///             `FormSetupAndMoveTo/4` (26), `GotoEnter/5` (22), `ExecCmd/4`
///             (22), `cmddelay/0` (8), `SetCommandOffset/2` (6),
///             `GetCommanded/0` (14), `SetCommanded/1` (2),
///             `ClearCommands/0` (2), `FormPathLeft/0` (2),
///             `GetCanExecCmd/1` (1)
///   free      `GetCmdStaminaCost/1` (42), `GetCmdCost/3` (22)
///
/// A member call may arrive on an `ObjList` as well as on an object --
/// `sq.Units.SetCommand("advance", pt)`, `ol.AddCommand(true, "advanceenter",
/// oTarget)`, `hero.army.SetCommand("idle")` -- and then applies to every
/// member in list order. The queue-mutating entry points all handle both; the
/// reads (`command`, `CmdCount`) do not, because no corpus site asks a list what
/// it is doing.
///
/// ### The cross-domain four
///
/// `GotoAttack(target, slice, flag[, give_up])` is `Goto` with the arrival
/// annulus taken from the attacker's own weapon rather than from an argument --
/// that is the only difference in the signature, and the shipped idiom
/// `while (!.GotoAttack(u, 1500, true, 15000)); while (.Attack(u));` fixes the
/// return value: it is true exactly when `Attack` would connect. So the range
/// comes from `CombatSystem::in_attack_range`'s own inputs -- the attacker's
/// `range` plus both radii, with the attacker's `min_range` as the inner bound
/// -- and not from a number invented here. With no combat system registered it
/// degrades to `Goto(target, 0, ...)`, which is a unit walking onto its target.
///
/// `GotoEnter(dest, range, slice, flag, give_up)` has `Goto`'s exact signature,
/// and `UNIT_ENTER.VS` puts the two in the arms of one `if`:
///
///     if (bEnterCatapult) while(!.Goto(pt, 0, 1000, true, 5000));
///     else                while(!.GotoEnter(pt, 0, 1000, true, 5000));
///
/// The difference is in `gbr.exe` (0x005d6620) rather than in the pairing:
/// **arrival is the band and nothing else**, and **`give_up` is how long the
/// call may go without a route before it ends the script** -- `0` at the first
/// failed search, never when negative. A walk is not timed. The details and
/// addresses are on `GotoOrder::enter`.
///
/// This used to read the pairing as "the end of a partial route counts as
/// arrival", inferred for a doorway the building's own footprint might block.
/// It was wrong twice over, and playtest #19 is what it cost: a route that was
/// never laid counted as arrival on the next call, and so did the end of a
/// partial one however far short it stopped, so `UNIT_BUILD_CATAPULT.VS`'s
/// builders called `AddUnit` on a machine from wherever their walk gave out --
/// across the sea from it, on Mediterranean, 1,900 units away. A door is a
/// standable point by construction (`sim/entrance.hpp`), so the case the
/// reading was for does not arise from a door.
///
/// `FormSetupAndMoveTo(dest, range, min_range, flag)` and `FormKeepMoving(ms)`
/// are the hero's march. `HERO_MOVE.VS` is the whole protocol:
///
///     .FormSetupAndMoveTo(pt, 0, 0, true);
///     while (.HasPath()) .FormKeepMoving(1500);
///
/// Setup lays the hero's own route and records the order; each `FormKeepMoving`
/// re-places the army at its formation offsets around the hero's *current*
/// position and facing, then suspends for the lesser of its argument and the
/// hero's time to arrival. The offsets come from `formation_offsets` and the
/// hero's `formation` -- both already in `sim/movement.hpp`, both read from
/// `DATA\FORMATIONS.XML`. Arguments two and three are the arrival annulus, which
/// `Hero.FormSetupAndMoveTo(b, 100, 100, false)` shows using both halves of.
///
/// ### The flag argument
///
/// `Goto`, `GotoEnter`, `GotoAttack` and `FormSetupAndMoveTo` each carry one
/// boolean, and `sim/movement.hpp` already records that no reading of it is
/// better supported than another. It stays recorded and unacted-on here for the
/// same reason, in all four, rather than acquiring a meaning in one of them.
std::size_t register_command_host(script::HostRegistry& registry);

/// How many entry points `register_command_host` defines. For asserting the
/// count at a call site without hard-coding a literal in two places.
[[nodiscard]] std::size_t command_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
