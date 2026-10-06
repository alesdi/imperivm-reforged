#pragma once

/// Squads: the `squad=<n>(<p>)` field every unit in the desync dumps carries.
///
/// ## What the dumps say
///
/// `docs/engine/state-vector.md` measures the field: the first component is an
/// index in `0 .. 91` where **0 means "no squad"**, the second is the owning
/// player, and `p` equals the player bit in `SyncFlags` in all 2,972 cases
/// where `n != 0`, with zero exceptions. So the player is *derived* and the
/// index is *per player*: index 4 exists simultaneously for players 0, 2, 3 and
/// 14 in the same dump. A squad is therefore keyed on the pair, and the pair is
/// what this file calls a `SquadKey`.
///
/// ## A hero's army is exactly one squad
///
/// Measured here, over the four dumps that contain heroes, on all 24 heroes
/// that have an army:
///
///   * every member of a hero's army prints the **same** `squad=<n>(<p>)` as
///     the hero itself -- 24 of 24, no exceptions;
///   * the squad's total membership is exactly `1 + army size` -- 24 of 24. The
///     51-member squads are a hero plus 50 attached warriors, and 50 is
///     `max_army` on `Hero.SC.XML`.
///
/// That is the whole shape: **a hero squad is the hero followed by its attached
/// warriors, and nothing else.** Squads that contain no hero exist too -- 129 to
/// 149 per dump -- and those never exceed 10 members in any of the nine dumps,
/// which is what `Squadize` builds for the AI.
///
/// ## How a script names a squad
///
/// A squad is **not** an object: it has no `ObjectId`, it never appears in a
/// dump's object block, and `SquadTable` is keyed on the `(index, player)` pair
/// above. So the handle a `script::Value` carries is that pair, packed into the
/// id of a `(TypeId, uint32)`: `(player << 16) | index`.
///
/// Packing rather than pooling, for the reason `sim/world_host.hpp` gives for
/// points: the key is already two small integers with no identity of their own,
/// so a pool entry would be a slot to allocate, to serialise, to sweep and to
/// keep reproducible across a save, in exchange for nothing. Packed, a squad
/// handle copies, compares, serialises and hashes with no side table at all,
/// and a suspended script holding one in a local slot survives a reload by
/// construction rather than by bookkeeping.
///
/// The engine's own entry points key squads the same way and are the reason to
/// trust that the pair is the whole identity: `gbr.exe` declares
/// `SelSquad: void, int nPlayerID, int nSquad` and `GetSquadCenter: point, int
/// nPlayerID, int nSquad`, and the script surface has `GetSquad(int, int)` and
/// `NumSquads(int)`. `Squad::No` returns the index and `Squad::Player` the
/// player, so a handle that carried only one of the two could not answer both.
///
/// A squad gets its own `TypeId` where a GAIKA does not (`sim/gaika.hpp`
/// carries that argument): `(index, player)` has no numeric meaning, and
/// `gbr.exe` never declares a squad argument as a bare `int` the way it
/// declares `int gaika` four times over.
///
/// ## The AI fields, and what is not claimed about them
///
/// `gbr.exe`'s `Squad::Dump` prints
///
///     Squad %d(%d)[%d/%d], SrcGAIKA: %d, GAIKAIn %d, DestGAIKA %d,
///     AIOrderDest %d, State %s, Flags: %04x, Strat: %s
///
/// and its `TVXSquad` persist block names `SquadLookIvl`, `Enemies`,
/// `StateSetTime`, `LastFight`, `DestGAIKA`, `GAIKAIn`, `SrcGAIKA`,
/// `ClassCount`. The fields below are the intersection of those two lists with
/// the members the corpus actually reads. They are **storage**: this file says
/// they exist and that they are world state, and says nothing about what
/// computes them.
///
/// One thing is claimed and it is quoted rather than inferred.
/// `DATA\AI\GS_SIEGE.VS` line 95 reads
///
///     if (squad.OrderDest != gaika) { squad.SendTo(gaika, 1); continue; }
///     // issue order, to force squad's AIDest to gaika
///
/// so `AIDest` follows the order destination. **`SendTo` is implemented now**
/// and is that writer: it posts an order on the player's AI order queue
/// (`AiOrderQueue`), after refusing a player whose AI is not running and a
/// squad carrying `SF_NOAI`, and `order_dest` and `ai_dest` are that order's
/// node. Its second argument is the order's priority. The order is carried out
/// later, by the queue's drain (`run_ai_orders`), which runs
/// `data/ai/AIOSendSquad.vs`.
///
/// ## Iteration order is state
///
/// Members are stored in a vector in attach order and squads in a vector sorted
/// by key. Rule 3 of docs/engine/architecture.md: an unordered container here
/// would make a formation's shape depend on allocation addresses.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::script {
class Scheduler;
}

namespace imperivm::core::sim {
class World;
struct Command;
}

namespace imperivm::core::sim {

/// The `squad=<n>(<p>)` pair.
///
/// `index == 0` is the dump's "no squad", and the dump prints `0(0)` for it --
/// all 417 disagreements between `p` and the owner bit are exactly those. So a
/// null key carries `kNoPlayer` internally but compares equal to nothing real.
struct SquadKey {
  std::int32_t index = 0;
  PlayerId player = kNoPlayer;

  [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }

  friend constexpr bool operator==(const SquadKey&, const SquadKey&) noexcept = default;
};

inline constexpr SquadKey kNoSquad{};

/// Ordering for the deterministic squad table: by player, then by index.
[[nodiscard]] constexpr bool squad_key_less(const SquadKey& a, const SquadKey& b) noexcept {
  if (a.player != b.player) return a.player < b.player;
  return a.index < b.index;
}

/// One squad: its key, its leader, and its members in join order.
///
/// `leader` is the hero for a hero squad and `kNoObject` for the leaderless AI
/// squads. It is **not** stored separately from `members`: the leader is the
/// first entry, which is what makes "the squad is the hero plus its army"
/// representable without a second list that could disagree with the first.
struct Squad {
  SquadKey key;
  ObjectId leader = kNoObject;
  std::vector<ObjectId> members;  ///< join order; the leader is [0] when there is one

  // -- the AI fields ---------------------------------------------------
  //
  // Storage, not behaviour. See the header note on what is and is not claimed.

  /// `Squad.State` -- 89 corpus sites, the most read member on the type. One of
  /// the `SS_*` values `DATA/AI/AI.INI` declares as data (`sim/ai_profile.hpp`),
  /// with `0` the engine-hardcoded `SS_IDLE` sentinel. Held as the raw integer:
  /// the names are a profile's to define, so nothing here may enumerate them.
  std::int32_t state = 0;
  /// `Squad.StateTime`. `TVXSquad` persists it as `StateSetTime`, so it is the
  /// game time the state was last set rather than a duration; `StateTime`'s
  /// three corpus sites all compare it against `GetTime()`.
  GameTime state_time = 0;

  /// `Squad.GetFlags` / `SetFlags` / `TestFlags`. Sixteen bits -- `Squad::Dump`
  /// prints `Flags: %04x` and `Squad::GetFlags` (0x004216d0) is a `movzwl` of
  /// this word. The `SF_*` names are engine-hardcoded rather than data: they
  /// are strings in `gbr.exe` where no `SS_*` name is.
  ///
  /// **Their values are no longer unknown**, which this comment used to say.
  /// `sim/globals.cpp` carries all five from the executable's own constant
  /// table -- `SF_NOAI` 1, `SF_ADVCHOOSER` 2, `SF_PEACEFUL` 4,
  /// `SF_WANTDRUIDS` 16, `SF_SENTRIES` 32 -- and bit 0 has a second,
  /// independent witness: `Unit::AI` (0x004254f5) reads exactly this word and
  /// returns the negation of bit 0. Two sources that could disagree and do not
  /// is what makes it a measurement rather than a transcription.
  ///
  /// Only `SF_NOAI` gets a constant below, because it is the only one any host
  /// body here reads. The rest stay values in the globals table for a script to
  /// pass in; the word is carried and masked, and what those bits mean is still
  /// the caller's.
  std::uint16_t flags = 0;

  /// The four GAIKA-valued fields `Squad::Dump` prints, in its order.
  ///
  /// `src_gaika` and `dest_gaika` and `gaika_in` are also three of the eight
  /// fields `TVXSquad` persists, which is what says they are world state and
  /// not a derived view.
  GaikaId src_gaika = kNoGaika;   ///< `Squad.SrcGAIKA`
  GaikaId gaika_in = kNoGaika;    ///< `Squad.GAIKAIn`
  GaikaId dest_gaika = kNoGaika;  ///< `Squad.DestGAIKA`
  GaikaId order_dest = kNoGaika;  ///< `Squad.OrderDest`; the dump's `AIOrderDest`
  /// `Squad.AIDest` -- 36 sites, the entry point this whole file exists to
  /// unblock. Stored rather than derived; see the header note.
  GaikaId ai_dest = kNoGaika;
  /// The squad's record on its player's AI order queue, `[squad+0x26]`, or -1.
  ///
  /// `OrderDest` and `AIDest` both read the destination **through this index**
  /// (0x00444140), so the two fields above are the record's node and nothing
  /// else: the queue's post writes all three together and its free clears all
  /// three together (`AiOrderQueue`). They are kept as fields rather than
  /// read through the queue because eight readers here take a `const Squad&`
  /// with no table beside it.
  std::int32_t order = -1;

  /// `Squad.LastFightTime`. `TVXSquad` persists it as `LastFight`.
  ///
  /// **Written when a member is struck**, by `record_squad_attacked` -- and
  /// written *once per tick*: `gbr.exe` 0x0041e6c0 compares the stored value
  /// against the current tick and returns early when they are equal, so what a
  /// squad remembers is the **first** blow of a tick and not the last.
  GameTime last_fight_time = 0;
  /// `Squad.GetLastAttacker` -- whoever landed that blow.
  ///
  /// Stamped in the same breath as `last_fight_time` and under the same
  /// once-per-tick rule, so the two never disagree about which blow they
  /// describe. `SQUADMONITOR.VS` reads them as a pair: *fought within the last
  /// three seconds, and the attacker is still valid, and it is a building or a
  /// sentry* -- which is how a squad decides it is being shot at by something
  /// it should walk away from.
  ObjectId last_attacker = kNoObject;

  /// `Squad.Eval` -- the squad's **strength**, and the input to `GAIKA::Eval`
  /// rather than an overload of it.
  ///
  /// `0x00421120` is a plain read of `[squad+0x1c]`, so it is stored rather
  /// than computed, which is what makes it a field here at all.
  ///
  /// **Nothing in this engine writes it**, the standing `ai_dest` above already
  /// has. That is not free: three of the nine shipped readers branch on it
  /// (`if (sq.Eval == 0)`, `coming += sq.Eval`), so a permanent zero makes the
  /// AI choose conservatively every time. The reader is bound anyway, because
  /// the alternative is a trap that stops `SQUADMONITOR.VS` at its 479 call
  /// sites instead of letting it run and choose. See `eval_impl`.
  std::int32_t eval = 0;

  [[nodiscard]] std::size_t size() const noexcept { return members.size(); }
  [[nodiscard]] bool empty() const noexcept { return members.empty(); }
  [[nodiscard]] bool contains(ObjectId id) const noexcept;
};

/// `Squad::AIDest` as 0x004215c0 answers it: the AI's current destination
/// (0x00444140, which `ai_dest` stores), and **`DestGAIKA` when there is
/// none**. Shared by the `AIDest` member and `GAIKA::ApproachingSquads`, which
/// asks the same question of every squad, so the two cannot disagree.
[[nodiscard]] inline GaikaId squad_ai_dest(const Squad& squad) noexcept {
  return squad.ai_dest != kNoGaika ? squad.ai_dest : squad.dest_gaika;
}

/// One record of the AI's order queue -- `CExecutionAI`'s `Todo`, 20 bytes in
/// the original, persisted as one blob per record under `order` (0x00449310).
///
/// 0x004494c0 fills it per verb: `verb` at +0, the squad's index at +4 and the
/// node at +6 for verb 1, and the priority at +0x10. **Only verb 1 is posted
/// here**; `AiOrderQueue` says why the other four are left out.
struct AiOrder {
  /// 0 is a free slot -- 0x00448a10 writes it, and the picker (0x00448ad0)
  /// skips it. 1 is *send squad*.
  std::uint16_t verb = 0;
  /// The squad's index, `(key >> 4) & 0xfff` in the original; the player is
  /// the queue's.
  std::int32_t squad = 0;
  /// The node the squad is sent to, `+6`.
  GaikaId node = kNoGaika;
  /// `+0x10`, sixteen bits: the script's `n`, set to 0 when the drain takes
  /// the record and never raised again. A record at 0 is never drained but
  /// still answers `OrderDest`.
  std::int16_t priority = 0;
  /// A free slot's link to the next one, `+4` reused; -1 ends the chain.
  std::int32_t next_free = -1;

  friend constexpr bool operator==(const AiOrder&, const AiOrder&) noexcept = default;
};

/// The AI's order queue, one per player: `CExecutionAI`, `[ai+0x28]`, which
/// `Squad::SendTo` posts to and a 500 ms timer drains.
///
/// ## What `gbr.exe` has
///
/// The object is constructed with the AI (0x00449250) and persisted with it
/// (0x00449310) under three names: `FirstFree`, the head of a free-slot chain
/// (`+0x10`, constructed -1), `View`, a point (`+0x14`, constructed -1,-1),
/// and `Todo`, the record array. Its destructor frees every record (0x00448e90
/// via 0x00448a10), which is what `AIStop` -- and an `AIStart` that replaces
/// an AI -- does to it.
///
///   * **The post** (0x004494c0) takes a verb, a priority and two words. For
///     verb 1 it first deletes the squad's previous record (0x00448a80), then
///     files the new one -- into the head of the free chain when there is one,
///     appended otherwise (0x00449280) -- and stores its index on the squad at
///     `[squad+0x26]` (`Squad::order`).
///   * **The free** (0x00448a10) writes the squad's index back to -1, links the
///     slot into the chain and zeroes its verb. Its priority is left where it
///     was. Three things free: the next post for the squad, `Squad::DelOrder`
///     (0x00421670), and the squad emptying (0x00444803).
///   * **The timer.** `CVXAI::Start` (0x0041e2d0) arms the AI object's timer 1
///     with no delay; its handler (0x0041d790) drains (0x00448dd0) and re-arms
///     it for 500 (0x00688240, which adds the delay to the clock now).
///   * **The drain** (0x00448dd0) picks one record (0x00448ad0), executes it
///     (0x00448b60), then ages the queue -- see `age`.
///   * **The pick** is over every record with a verb and a priority above 0,
///     in index order, and keeps a candidate only when it is strictly better
///     (0x004488e0): a larger `priority / 5` (signed, truncating), and within
///     one such band, a squad whose front member stands nearer `View`.
///     `View` has no writer anywhere in the order code or behind any load of
///     `[ai+0x28]` that was scanned, so it stays at (-1, -1) and the tie goes
///     to the squad nearer the map's top-left corner -- **read, not assumed;
///     the absence of a writer is a scan, not a proof.** Ties beyond that keep
///     the lower index.
///   * **The execution** of verb 1 (0x00448b60): nothing at all while the AI's
///     script slot 2 still runs the previous one (0x0069f950). Otherwise the
///     record's priority goes to 0, its squad goes in a `SquadList`, and so
///     does the squad of **every other verb-1 record with a priority above 0,
///     bound for the same node, whose front member stands within 240 of this
///     one's** (0x00417990, the floored root, unsigned compare) -- each taken
///     the same way, priority 0. Every squad put in the list has its
///     `SrcGAIKA` set to its `GAIKAIn` (`[squad+0x20] = [squad+0x22]`). Then
///     `data/ai/AIOSendSquad.vs` is spawned into slot 2 (0x006a07e0) with the
///     list and the node.
///
/// ## The verbs left out
///
/// The drain handles five verbs, each a script spawned into slot 2: 1
/// `AIOSendSquad.vs`, 2 `AIOTrain.vs` (with a copied unit-type string the
/// drain frees after), 3 `AIOUpgradeSet.vs`, 4 `AIOSupplySet.vs`, 5
/// `AIOSupplySquad.vs`. **Only verb 1 is reachable.** The post has six
/// callers: `Squad::SendTo` (0x004215a7), the regroup (0x0044755d), the
/// squad maker 0x004471d3 behind it, all verb 1; `SendSquadToLsaX`
/// (0x0042e170) and `AIO_SendSquad` (0x00421c1c), verb 1 again; and
/// `AIO_Train` (0x00421c98), the only verb 2. No shipped script, in the packs
/// or in any map container, calls `AIO_Train`, `AIO_SendSquad` or
/// `SendSquadToLsaX`, and nothing posts verbs 3 to 5 at all. Their scripts
/// agree: `AIOTrain.vs` returns before its body ("obsolette"), and the other
/// three are a commented-out `pr`. So `AiOrder` carries verb 1's fields and
/// nothing posts any other.
///
/// ## What is inferred, labelled
///
///   * **Where the timer fires in a turn.** This engine has no timer queue;
///     `run_ai_orders` fires a due timer once per turn, after the systems have
///     advanced and before the scheduler's pass, at the turn's time. A spawned
///     runner therefore runs in that turn's pass. The original orders object
///     timers and scripts by their due times inside one pump turn; which of the
///     two runs first on a shared tick was not read.
///   * **Slot 2's `Context`** (0x0041ccb0 names it after the player) is not
///     kept: `AIOSendSquad.vs` reads no environment key, so nothing observes it.
///   * **A squad empties** is the free here; the original frees when the
///     squad's running strength reaches zero (0x004447f6), which is the same
///     moment for every squad whose last member has any strength.
///
/// ## Hashed
///
/// Unlike the squad's own AI fields (see `SquadTable::hash`): the queue decides
/// which script a peer spawns and when, so two peers that disagree about it
/// diverge one drain later in hashed state anyway, and hashing it names the
/// cause rather than the symptom.
struct AiOrderQueue {
  std::vector<AiOrder> todo;  ///< `Todo`; the index is the record's identity
  std::int32_t first_free = -1;  ///< `FirstFree`
  Point view{-1, -1};            ///< `View`; see the note on the pick
  /// When the AI's timer 1 next fires. 0 is "at the next pass", which is the
  /// state `CVXAI::Start` arms it in, so a reset queue needs no arming.
  GameTime due = 0;
  /// The coroutine in the AI's script slot 2 -- the last order the drain
  /// spawned -- or `kNoScript`. While it is alive the drain executes nothing.
  script::ScriptId runner = script::kNoScript;

  /// The constructed state: what `AIStop` leaves and `AIStart` begins from.
  void reset() noexcept { *this = AiOrderQueue{}; }
  friend bool operator==(const AiOrderQueue&, const AiOrderQueue&) noexcept = default;
};

/// The timer's period, 0x0041d7f6.
inline constexpr GameTime kAiOrderPeriod = 500;
/// The batching radius of the drain, 0x00448c68.
inline constexpr std::int64_t kAiOrderBatchRadius = 240;

/// Every squad in the world.
///
/// Squads are kept sorted by `squad_key_less` so that iteration -- and
/// therefore hashing, and therefore the conformance comparison -- does not
/// depend on the order they were created in.
class SquadTable {
 public:
  /// The lowest index not currently in use by `player`, starting at 1.
  ///
  /// **Inferred.** The dumps show indices 0 to 91 with 85 distinct values, which
  /// is consistent with lowest-free reuse and with a monotone counter alike;
  /// nothing distinguishes them. Lowest-free is chosen because it keeps indices
  /// inside the observed range over a long session, and because a squad index is
  /// per-player and 92 values would overflow a monotone counter quickly.
  [[nodiscard]] std::int32_t next_free_index(PlayerId player) const;

  /// Create an empty squad with a freshly allocated index for `player`.
  SquadKey create(PlayerId player, ObjectId leader = kNoObject);

  /// Create a squad at a specific key, for loading a saved or dumped world.
  /// Returns false if that key is already taken.
  bool create_at(SquadKey key, ObjectId leader = kNoObject);

  /// Add `id` to the squad, at the end. A no-op if it is already a member.
  /// Returns false when the key names no squad.
  bool join(SquadKey key, ObjectId id);

  /// Remove `id` from the squad, preserving the order of the rest. Removing the
  /// leader leaves the squad leaderless rather than promoting anybody: the
  /// dumps have no evidence for promotion, and `DetachArmy` disbands instead.
  ///
  /// The last member leaving frees the squad's AI order on the spot, as
  /// 0x00444803 does, so a post that follows in the same breath -- the
  /// regroup's -- takes the slot it left.
  bool leave(SquadKey key, ObjectId id);

  /// Remove the squad entirely, and its AI order with it.
  bool destroy(SquadKey key);

  [[nodiscard]] const Squad* find(SquadKey key) const;
  [[nodiscard]] Squad* find(SquadKey key);

  /// The squad `id` belongs to, or `kNoSquad`. Linear over squads; the dumps'
  /// worst case is 162 squads, and a side index would be a second thing to keep
  /// in step with the first.
  [[nodiscard]] SquadKey squad_of(ObjectId id) const;

  [[nodiscard]] std::span<const Squad> squads() const noexcept { return squads_; }

  /// The same span, writable. **For `revalue_squads` and the save reader
  /// only** -- a squad's AI fields are the AI's to write, which is why every
  /// other caller gets the const span above.
  [[nodiscard]] std::span<Squad> mutable_squads() noexcept { return squads_; }
  [[nodiscard]] std::size_t size() const noexcept { return squads_.size(); }
  [[nodiscard]] bool empty() const noexcept { return squads_.empty(); }

  /// Drop every squad that has no members left. `delete_empty` on the item
  /// holders has the same flavour; an empty squad is not a thing the dumps ever
  /// print.
  void prune_empty();

  void clear() noexcept {
    squads_.clear();
    for (AiOrderQueue& queue : orders_) queue.reset();
  }

  // -- the AI order queue -------------------------------------------------
  //
  // `AiOrderQueue` carries the reading. Held here rather than on `AiSystem`,
  // whose object the original hangs it from, because a record is freed the
  // moment its squad empties and every way a squad loses a member is a method
  // of this table: a queue anywhere else would need to be told, from eleven
  // call sites, or would find out late and reuse its slots in another order.

  /// `player`'s queue, or null outside the table.
  [[nodiscard]] const AiOrderQueue* orders(PlayerId player) const noexcept;
  [[nodiscard]] AiOrderQueue* mutable_orders(PlayerId player) noexcept;

  /// 0x004494c0, verb 1: free `key`'s previous record, file a new one for
  /// `node` at `priority` on `key.player`'s queue -- in the head of the free
  /// chain, or appended -- and point the squad at it, which is what
  /// `OrderDest` and `AIDest` then answer. False when `key` names no squad.
  /// Whether the player's AI is running is the caller's to ask.
  bool post_order(SquadKey key, GaikaId node, std::int16_t priority);

  /// 0x00448a80: free `key`'s record, if it has one. True when it had one.
  bool delete_order(SquadKey key);

  /// The queue's destructor (0x00448e90): every record freed, every squad of
  /// `player` pointed at none, and the queue back to its constructed state.
  void reset_orders(PlayerId player);

  /// Fold the whole table into a world hash, in table order.
  ///
  /// **Membership only** -- key, leader, members. The AI fields on `Squad` are
  /// deliberately left out, for the reason `sim/command.hpp` gives for leaving
  /// the command queue out: the original's desync dumps carry squad membership
  /// (every unit prints `squad=<n>(<p>)`) and carry no squad state at all. The
  /// `SrcGAIKA` / `GAIKAIn` / `DestGAIKA` / `State` / `Flags` line is
  /// `Squad::Dump`, a debug console command, and appears in none of the nine
  /// dumps in the retail install. Hashing them would pull a subsystem into the
  /// determinism contract that the shipped build left out of it. They are still
  /// world state and still have to survive a save; hashing and saving are not
  /// the same list.
  ///
  /// **The AI order queues are folded after the squads**, every record and the
  /// timer and runner of each -- `AiOrderQueue` says why that structure is the
  /// exception.
  void hash(std::uint64_t& accumulator) const noexcept;

  // -- the saved game ----------------------------------------------------

  /// Append the whole store to `out`, with its own magic and version.
  ///
  /// A pair rather than a rebuild through `create_at`/`join`, because a squad
  /// carries `state`, `flags`, four GAIKA references and `last_fight_time`, and
  /// none of those has a public setter -- deliberately, they are the AI's to
  /// write. The vector is written in its own order, which is the sorted order
  /// `lower_bound` depends on and which iteration makes state.
  ///
  /// Definitions in `src/sim/save_systems.cpp`, next to the systems that own
  /// this store. Layout: docs/formats/save.md.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace it with the one in `bytes`. **Atomic**: decoded into a local and
  /// moved in only once every row has read cleanly.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  [[nodiscard]] std::size_t lower_bound(SquadKey key) const noexcept;
  /// 0x00448a10: the squad's record back on the free chain, and the squad
  /// pointed at none.
  void free_order(Squad& squad) noexcept;

  std::vector<Squad> squads_;  ///< sorted by squad_key_less; iteration order is state
  std::array<AiOrderQueue, kPlayerCount> orders_{};  ///< by player; see `AiOrderQueue`
};

/// Bring every squad's `eval` and `gaika_in` up to date -- **the two stored
/// fields nothing in this engine used to write**, and the reason nine shipped
/// readers all answered zero.
///
/// ## What the original does, and why one pass here is the same answer
///
/// `Squad::Eval` is **maintained, not computed**: 0x0041e900 adds one unit's
/// census valuation (`object_power`, 0x004439d0, sixteen bits of it) to
/// `[squad+0x1c]` and 0x0041e890 takes it away again, and between them they are
/// called from exactly three places -- `Unit::SetHealth` (0x005d3a80, which
/// subtracts before the write and adds after), the class-derived stat recalc
/// behind `vtbl+0xc4` (0x005d9e90, same bracket), and the member walk of
/// `Squad::RemoveMember` (0x004447xx, which subtracts the leaver and zeroes the
/// field outright when the squad empties). Every term of the valuation --
/// class damage and slash armour, the attack bonus, the effective level, health
/// and maximum health -- can only change through one of those, so the stored
/// number **is** the sum of the members' current valuations at all times.
/// Summing it once a turn therefore lands on the same value; what it gives up
/// is *when*, and that is named below.
///
/// `Squad::GAIKAIn` is the node the squad's **leader** stands in. 0x0041f530
/// runs on every unit move: it asks `GetGAIKA(point)` (0x0044e3f0, which is
/// `GaikaTable::at`) for the node under the new position and, when the mover is
/// the squad's leader and that node differs from where the squad is filed,
/// 0x0041eac0 unfiles the squad from the old node's list, files it under the
/// new one and writes the id into `[squad+0x22]`. The same wrapper stamps
/// `SrcGAIKA` from it **the first time it is anything but zero**, which is why
/// a squad remembers where it was raised.
///
/// ## What is approximated
///
///   * **Once a turn, not on the instant.** The original updates inside the
///     move and inside the health write; this runs from `HeroSystem::advance`.
///     A script that reads `sq.Eval` after damaging a unit *within the same
///     turn* sees the value from the turn's start. Nothing shipped does that:
///     the nine readers are all AI polling loops that sleep between passes.
///   * **The "leader" the node follows is the member deque's front**
///     (0x00443df0), which is `Squad::leader` whenever there is one and the
///     next member along when a leader has left without being replaced -- so a
///     squad never stops tracking somebody. The original's other rule here --
///     an emptied squad is filed under no node and then destroyed -- is
///     `prune_empty`, which `HeroSystem::advance` runs first, so there is never
///     an empty squad left for this to file.
///   * **The unit flag at bit 23 of `[unit+0x194]` is not modelled.** The
///     original refuses to adjust the running total for a unit carrying it,
///     which leaves that unit's contribution frozen at whatever it was when it
///     joined; here every member is valued alike.
void revalue_squads(World& world, SquadTable& squads);

class ScriptLibrary;

/// The AI order queues' timer, once a turn: for every player whose AI is
/// running and whose timer is due at `now`, drain one order and re-arm the
/// timer for `now + 500` (0x0041d790). `AiOrderQueue` carries the reading of
/// the drain; `GameSession::advance` calls this after the systems and before
/// the scheduler's pass, which is the labelled part. `library` compiles
/// `AIOSendSquad.vs` on first use and may be null, in which case the
/// scheduler's own chunks are searched. Returns how many runners it spawned.
std::size_t run_ai_orders(World& world, script::Scheduler& scheduler, ScriptLibrary* library,
                          GameTime now);



// --------------------------------------------------------------------------
// the script handle
// --------------------------------------------------------------------------

/// `SF_NOAI`, bit 0 of `Squad::flags`.
///
/// Named here and not left to the globals table because `Obj::AI` is defined in
/// terms of it: 0x004254f5 loads the squad flags word and returns
/// `!(flags & 1)`. `sim/globals.cpp` declares the same value for the script
/// side, and the two are independent -- one decoded from the executable's
/// constant table, one read off the instruction.
inline constexpr std::uint16_t kSquadFlagNoAi = 1;

/// `SF_PEACEFUL`, bit 2 of `Squad::flags`.
///
/// Named here for the same reason `SF_NOAI` is: a host body reads it. It is the
/// one bit `GAIKA::GetAIControlledUnits`'s boolean argument gates -- `false`
/// refuses a peaceful squad's members, `true` takes them -- which is the whole
/// difference between the temple recruiter and the other three. The value comes
/// from the executable's own constant table, which `sim/globals.cpp` also
/// declares for the script side.
inline constexpr std::uint16_t kSquadFlagPeaceful = 4;

/// `SF_ADVCHOOSER`, bit 1 -- the same table. Named here because
/// `Ship::ApplyAiTransport` clears it on every squad it lands: a crossed army
/// stops choosing its own route and takes the crossing's order.
inline constexpr std::uint16_t kSquadFlagAdvChooser = 2;

/// `SF_SENTRIES`, bit 5 -- `sim/globals.cpp` carries all five values from the
/// executable's own constant table. Named here because `MilEval` masks it out
/// with the two above: 0x25 is the whole of "not fielded strength".
inline constexpr std::uint16_t kSquadFlagSentries = 32;

/// A squad's handle type. `kTypeObj`=1, `kTypePoint`=2, `kTypeQuery`=3,
/// `kTypeSettlement`=4 (`sim/world_host.hpp`), `kTypeObjList`=5
/// (`sim/objlist.hpp`). A GAIKA takes none; see `sim/gaika.hpp`.
inline constexpr script::TypeId kTypeSquad = 6;

/// The `TypeId` a `SquadList` handle carries. **11**, continuing the census
/// `sim/world_host.hpp` keeps: 1-4 and 8 are its, 5 `sim/objlist.hpp`'s, 6 this
/// file's, 7 `sim/globals.hpp`'s, 9 and 10 `sim/array.hpp`'s.
inline constexpr script::TypeId kTypeSquadList = 11;

/// A pooled list's handle. Zero is never issued, so a default-constructed
/// value is distinguishable from a real empty list -- `ObjListId`'s rule.
using SquadListId = std::uint32_t;

inline constexpr SquadListId kNoSquadList = 0;

/// **Bit 3 of `Squad::flags`, and the reason the `SF_*` values have a hole in
/// them.**
///
/// The five names the executable's constant table carries are 1, 2, 4, 16 and
/// 32 -- `SF_NOAI`, `SF_ADVCHOOSER`, `SF_PEACEFUL`, `SF_WANTDRUIDS`,
/// `SF_SENTRIES` -- and 8 is missing from the run. It is missing because it is
/// not a script's to set: `Squad::Lock` (0x004217e0) is `or byte ptr
/// [squad+0x30], 8` and `Squad::Unlock` (0x00421820) is `and ... 0xf7`, on the
/// same word `Squad::GetFlags` reads with a `movzwl` (0x004216fd). The
/// `SquadList` forms (0x0042bce0 and 0x0042bde0) are the same two writes
/// applied to every squad in the list.
///
/// **What honours it is the AI, not this engine.** The bit has exactly two
/// readers in `gbr.exe`, both inside the squad-management code at 0x00446c36
/// and 0x00446d89, where a squad is picked over for disbanding or merging. So
/// the lock means *the script is walking this list, leave these squads alone*,
/// and there is nothing here for it to stop yet -- `SquadTable::prune_empty` is
/// the only thing that removes a squad and it runs from a system, not from a
/// script's slice. The bit is still written, because `GetFlags` and `TestFlags`
/// can see it and because the day an AI here disbands squads this is what it
/// will have to consult.
inline constexpr std::uint16_t kSquadLocked = 0x0008;

[[nodiscard]] script::Value make_squadlist_value(SquadListId id) noexcept;
[[nodiscard]] bool is_squadlist(const script::Value& value) noexcept;
/// `kNoSquadList` when `value` is not a `SquadList` handle.
[[nodiscard]] SquadListId squadlist_of(const script::Value& value) noexcept;

/// The squad lists every live script holds.
///
/// `ArrayPool`'s shape rather than `ObjListPool`'s: entries keyed by
/// declaration site, released with the script, serialised and **not hashed**.
/// There are no aliases and no mark-and-sweep because a `SquadList` is never
/// somebody else's storage and no host function returns one -- `GetSquads`
/// takes the caller's by reference and fills it, which is why the corpus
/// declares `SquadList SL;` and never assigns one.
///
/// ## A list is a cursor as well as a list
///
/// `Cur`, `Next`, `EOL` and `Rewind` are the whole of how the corpus reads one:
/// `SL.Lock; while (SL.EOL == false) { squad = SL.Cur; SL.Next(); ... }
/// SL.Unlock;`. The original keeps the cursor *in* the list object -- the
/// deque at `[list]` and an iterator pair at `[list+4]`/`[list+8]` -- so it is
/// state the list carries and not something the caller supplies, and two
/// scripts walking one list would tread on each other. Nothing in the corpus
/// shares one, and the by-reference `GetSquads` is why: a list is a local.
///
/// **The cursor is not hashed either.** It is a script's own bookkeeping, the
/// same standing `scriptstate` has in all nine dumps; it is serialised so that
/// a save taken mid-walk resumes mid-walk rather than at the beginning.
class SquadListPool {
 public:
  /// Mint or reuse the entry for one declaration site, cleared and rewound.
  SquadListId acquire(script::ScriptId script, std::uint32_t slot);

  /// Drop every entry a script owns. Called from the scheduler's teardown hook.
  void release_script(script::ScriptId script);

  /// Give the live entry `id` to `script`, filed under `slot`: for a launcher
  /// that builds a list before the script it hands it to exists -- the AI
  /// order drain, whose runner's id is the spawn's answer. The entry then goes
  /// when that script does.
  void hand_to(SquadListId id, script::ScriptId script, std::uint32_t slot) noexcept;

  [[nodiscard]] bool contains(SquadListId id) const noexcept;
  [[nodiscard]] script::ScriptId owner_of(SquadListId id) const noexcept;

  /// Empty for an unknown handle, which is what a stale list should read as
  /// rather than a trap: `SL.EOL` on one is true, not an error.
  [[nodiscard]] std::span<const SquadKey> items(SquadListId id) const noexcept;
  /// Null for an unknown handle.
  [[nodiscard]] std::vector<SquadKey>* mutable_items(SquadListId id) noexcept;

  /// How far through the list the cursor is. `size()` means at the end.
  [[nodiscard]] std::size_t cursor(SquadListId id) const noexcept;
  void set_cursor(SquadListId id, std::size_t at) noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }

  /// Little-endian, self-describing, versioned, like every other store here.
  void serialize(std::vector<std::byte>& out) const;
  [[nodiscard]] Status deserialize(std::span<const std::byte> data);

  void clear() noexcept { entries_.clear(); }

 private:
  struct Entry {
    script::ScriptId script = script::kNoScript;
    std::uint32_t slot = 0;
    bool live = false;
    std::size_t cursor = 0;
    std::vector<SquadKey> members;
  };

  std::vector<Entry> entries_;
};

/// The pool a world holds. Declared here so that `sim/world.hpp` needs only the
/// type, and defined in `sim/squad.cpp` beside everything else that uses it.
[[nodiscard]] SquadListPool& squadlist_pool_of(World& world);

/// Pack a squad key into a script value: `(player << 16) | index`.
///
/// `kNoSquad` -- and any key whose index is 0 -- packs to the invalid-handle
/// value `(kNoType, 0)`, which is the convention `WorldHost::default_value`
/// already uses for every other handle type, so `.IsValid` and `Host::truthy`
/// report on it without a special case.
[[nodiscard]] script::Value pack_squad(SquadKey key) noexcept;

/// Unpack one. Anything that is not a squad handle gives `kNoSquad`.
[[nodiscard]] SquadKey unpack_squad(const script::Value& value) noexcept;

[[nodiscard]] bool is_squad(const script::Value& value) noexcept;

/// The squad a receiver names, or null: a squad handle naming a live squad in
/// `world`'s hero system, and nothing else.
///
/// **For the bodies that serve `Obj` and `Squad` under one name.** `gbr.exe`
/// registers `pos`, `player`, `health`, `maxhealth`, `food`, `InHolder` and
/// `HasFreedom` on `Squad` as well as on `Obj` (0x00422cf0, 0x004210c0,
/// 0x00427740, 0x00427800, 0x004278c0, 0x00427670, 0x00427aa0), and this
/// registry keys on (kind, name, arity) with no receiver type, so the handle
/// type picks the half inside one body -- the rule `Size` and `SetState`
/// already follow. Each of those bodies asks this first.
[[nodiscard]] const Squad* squad_receiver(World& world, const script::Value& value) noexcept;

/// `Squad::SetCmd`'s core (0x0043ec00): the state and its stamp, the flags
/// masked set-then-clear, and `verb` with `prototype` replacing every member's
/// queue in join order. Shared by the three `SetCmd` overloads and by
/// `Ship::ApplyAiTransport`, which runs it on every squad it lands.
void squad_set_cmd(World& world, Squad& squad, std::int32_t state, std::int32_t set_flags,
                   std::int32_t clear_flags, std::string_view verb, const Command& prototype,
                   GameTime now);

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

/// Implement the squad slice of the `.vs` host API.
///
/// Exactly three entry points, and the two `SetCmd` arities are one function:
///
///   `SetCmd/4`, `SetCmd/5` (30 sites), `AIDest/0` (36).
///
/// Both were blocked on the representation above rather than on any question
/// about what they do, which is why they are here and the rest of the `Squad`
/// surface is not.
///
/// ### `SetCmd`, and where its shape comes from
///
/// `gbr.exe`'s signature table gives it exactly, receiver included:
///
///     Squad::SetCmd     void, Squad sq, int nState, int nSetFlags,
///                       int nClrFlags, str cmd
///     Squad::SetCmdObj  void, Squad sq, int nState, int nSetFlags,
///                       int nClrFlags, str cmd, Obj obj
///                       void, Squad sq, int nState, int nSetFlags,
///                       int nClrFlags, str cmd, point pt
///     Squad::ClrCmd     void, Squad sq, int nState, int nSetFlags,
///                       int nClrFlags
///
/// so the fifth VS argument is an object *or* a point, which is the same pair
/// `AddCommand` and `SetCommand` take, and `nSetFlags` / `nClrFlags` are a mask
/// pair rather than one value. The corpus bears the argument order out:
/// `squad.SetCmd(SS_KillAll, 0, SF_ADVCHOOSER, "ai_killall")` sets nothing and
/// clears `SF_ADVCHOOSER`, which is what 27 of the 30 sites do.
///
/// It composes three things, and only the third is inferred:
///
///   1. **the state** -- `nState`, with `state_time` set to now. Measured:
///      `ClrCmd` has the same three leading arguments and no command at all, so
///      state and flags are what the two share.
///   2. **the flags** -- `(flags | nSetFlags) & ~nClrFlags`. Measured the same
///      way, and by the argument names in the signature.
///   3. **the command** -- `cmd` is issued to **every member of the squad, in
///      join order, with `SetCommand`'s semantics**: the queue is replaced and
///      whatever was running is aborted.
///
/// Three corpus facts pin (3) down. `cmd` names a `<method sig>` and not a
/// `<cmd name>`: `ai_killall` is bound by `UNIT.SC.XML` and `HERO.SC.XML` and
/// appears in no `DATA\COMMANDS\*.XML` file, so this is `SetCommand`'s name
/// space, not `ExecCmd`'s. It reaches the members rather than the leader:
/// `SQUADMONITOR.VS` line 324 is `sq.SetCmd(SS_IDLE, 0, SF_ADVCHOOSER, "move",
/// sqLeader.pos)`, which orders the squad to the leader's *own* position and
/// would be a no-op if it only reached the leader. And it replaces rather than
/// appends: `SQUADMONITOR.VS` line 79 writes `sq.Units.AddCommand(false,
/// "advance", ...)` when it wants an append, going through `Units` to get at a
/// member `Squad` does not have -- so `SetCmd` is the other one.
///
/// The `Set`/`Clr` naming and `SetCommand`'s own documented behaviour
/// (sim/command.hpp) are what make "replaces and aborts" the reading rather
/// than "appends"; a recording of an AI squad interrupted mid-`engage` by a
/// `SetCmd` would settle it directly.
///
/// ### `AIDest`
///
/// A read of `Squad::ai_dest`, which `SendTo` writes and nothing else does --
/// see the header note. On a squad nobody has sent anywhere it answers
/// `kNoGaika`, which is `0`, which is exactly what `SQUADMONITOR.VS`'s
/// `if (sq.AIDest > 0)` and `if (sq.AIDest == 0)` guards are written to
/// handle.
///
/// ### What is not here, and why
///
/// `DelOrder/0`, `ClrCmd/3`, `SetState/1`, `State/0`, `Player/0`,
/// `Size/0`, `Leader/0`, `Units/0`, `TestFlags/1`, `Eval` and
/// `Count` are all reachable now that a squad has a handle, but **member
/// lookup is case-insensitive**, so `Player/0`, `pos/0`, `health/0`,
/// `maxhealth/0`, `food/0`, `Count/2` and `InHolder/0` collide exactly with
/// entry points `world_host`, `combat`, `economy` and `objlist` already define
/// for objects. Each of those has to grow a squad branch inside the existing
/// implementation rather than acquire a second `define()`, which would replace
/// the first silently. That is a change in files this slice does not own.
///
/// Returns the number of entry points defined, the convention every other
/// `register_*_host` follows, so a caller can assert the count rather than
/// trust it.
class World;
class HeroSystem;

/// 0x00447330, the regrouping `SquadList::Train` ends with: every unit in
/// `units` leaves the squad it was in and gets a fresh squad of its own,
/// carrying the old squad's flags, `SrcGAIKA`, `DestGAIKA` and
/// `LastFightTime`, and its AI order re-posted at the priority it has --
/// or, for a unit with no squad, the flags its class
/// implies (`SF_PEACEFUL` for a `Peaceful` or `Animal`, `SF_SENTRIES` for a
/// `Sentry`) and the node under its feet -- with bit 0 rewritten from the
/// unit's own no-AI flag and the lock bit cleared. `state` and `now` are
/// written last, on every squad made. A unit attached to a hero stays in
/// the hero's squad untouched; a hero keeps its own squad and that squad
/// takes the state. Empty squads are pruned. See `train_impl` for the
/// evidence and the one labelled reading.
/// `AddToSquad(unit, dest)` -- 0x00446a70, the rule `Squadize` and the spawn
/// hook share; `sim/squad.cpp` carries the seven arms. Answers the squad the
/// unit is in afterwards, or `kNoSquad` for a unit that belongs nowhere.
SquadKey add_to_squad(World& world, HeroSystem& heroes, ObjectId id, GaikaId dest);

/// 0x0041e820, the hook the object manager runs when a unit enters the world,
/// run over every unit with an id above `after` that is alive, owned and in no
/// squad: each is put through `add_to_squad` toward the node under it. Only
/// while the AI manager exists (`AiSystem::manager_started`), which is the
/// original's own gate. Returns the highest id looked at, for the caller to
/// pass back next turn. `HeroSystem::advance` is the caller.
ObjectId enrol_new_units_in_squads(World& world, HeroSystem& heroes, ObjectId after);

void regroup_into_fresh_squads(World& world, HeroSystem& heroes, std::span<const ObjectId> units,
                               std::int32_t state, GameTime now);

std::size_t register_squad_host(script::HostRegistry& registry);

/// How many entry points `register_squad_host` defines.
[[nodiscard]] std::size_t squad_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
