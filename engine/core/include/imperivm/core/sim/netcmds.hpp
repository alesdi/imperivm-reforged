#pragma once

/// The net command stream: what a lockstep match exchanges, and the order in
/// which every peer applies it.
///
/// This is the half of multiplayer that is not networking. Two peers running
/// the same simulation from the same seed stay in step only if they apply the
/// same player orders, on the same turn, **in the same sequence** -- and that
/// last clause is the whole of this header. Sockets decide when a turn's
/// orders are complete; this decides what a turn's orders mean once they are.
///
/// ## What an order is
///
/// `sim/orders.hpp` already answers that, from `GROUP_UNITSOUT.VS`'s own
/// signature:
///
///     //void, ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier, int player
///
/// an actor list, a target that is always a point and sometimes also an
/// object, two booleans and the issuing player. A `NetOrder` is that tuple and
/// nothing else, because that is what the original passes and therefore what a
/// peer has to be told.
///
/// ## The ordering rule, which is this engine's
///
/// A turn's orders arrive from several peers and no arrival order is shared,
/// so the sequence has to be recomputed from the orders themselves. The rule
/// here is **by issuing player, then by the order's own sequence number within
/// that player** -- a total order every peer computes from the same data
/// without talking.
///
/// `docs/engine/state-vector.md` records what the original's pump logged:
/// `CmdNotUI, idx: n` with `n` in 0..8 immediately before each command, and
/// "the index correlates with player slot in the samples but is not
/// confirmed". That is consistent with sorting by player and is not proof of
/// it, so the rule above is labelled as this engine's rather than read. What
/// matters for correctness is only that it is *total* and *computed*: any
/// rule both peers apply keeps them in step, and a rule that depends on
/// arrival order keeps neither.
///
/// ## Why the stream hashes
///
/// `netcmds` is one of the nine channels in `[HASHES]`, non-zero in seven of
/// the nine retail dumps and zero in exactly the two whose `cmdsprocessed` is
/// zero -- so the shipped build hashed the command stream alongside the object
/// slots and the script threads. It did that because a desync caused by
/// *disagreeing about the orders* is otherwise indistinguishable from one
/// caused by simulating them differently, and the two have entirely different
/// causes.
///
/// `stream_hash` here is that fold, over the fields a `NetOrder` carries.
/// **It is not the original's.** No dump prints a command payload
/// (`state-vector.md`: "The payload is not printed -- this is the reason these
/// files cannot be replayed"), so the original's algorithm cannot be
/// recovered and the recorded values cannot be reproduced. What this fold
/// buys is the real half: two peers that disagree about a turn's orders say so
/// on that turn, instead of drifting apart later somewhere in the simulation.
///
/// **It is deliberately not folded into `World::hashes()`.** Doing that means
/// registering a system, which changes `slots` for every scenario and moves
/// the conformance golden; the channel is compared beside the world's hashes
/// by `conformance::check_lockstep` instead. See the plan.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

/// What kind of thing a player did.
///
/// A right click is the order `GROUP_UNITSOUT.VS`'s signature describes, and
/// the only kind the stream carried at first. The interface does two other
/// things that change the simulation, and a lockstep match has to carry them
/// as orders too or the peer that did them plays a different game:
enum class NetOrderKind : std::uint8_t {
  /// A right click: the verb comes from the class graph at apply time.
  default_order,
  /// A command bar row, by its `<cmd name>`. Pressed when `aimed` is false;
  /// aimed at `target` when it is true -- the second click of a row that asks
  /// for one.
  command,
  /// The game menu's surrender: `declare_match(issuer, lost)`.
  surrender,
  /// `issuer` has left, and this is the first turn without it: the computer
  /// takes its seat. **Never on the wire** -- no peer can say another has
  /// left; the negotiator writes it into the agreed turn the match decided
  /// (`sim/netdepart.hpp`), so it is in the history, the stream hash and any
  /// replay of them like every other order.
  departed,
  /// `SetSpeed`: the game speed, per mille, from the turn after this one on
  /// every peer. The original never writes the speed where it is asked for:
  /// `SetSpeed` (0x004c6ce0) posts a `CVXCmdSetSpeed` command with the local
  /// player as issuer, execution (0x004e67c0) clamps it to 1..100000 and
  /// writes the clock's speed on every peer, and the clock (0x00528a80) turns
  /// real time into game time with it from the next turn.
  set_speed,
  /// `issuer`'s seat, which the computer has played since its player left,
  /// has a player again from this turn: a late joiner (`sim/netjoin.hpp`).
  /// The computer stops here. **This engine's**, as the late join is: the
  /// original admits nobody once a match has started. Like `departed`,
  /// **never on the wire** -- the negotiator writes it into the first turn of
  /// the joiner's part, which the host decided -- so it is in the history,
  /// the stream hash and every replay of them.
  joined,
  /// The Diplomacy screen's OK: `issuer`'s relations word for `other`, or,
  /// with `other` = `kNoPlayer`, `issuer`'s allied-victory flag. The screen
  /// never writes the matrix itself: its OK (0x006cb680) builds, per row
  /// whose word changed, a command of four fields -- `player1` the local
  /// player, `player2` the row's, `relations` the whole new word, `allied`
  /// (their names are the serialiser's, 0x004e6030) -- and posts it through
  /// the local command path (0x0051d3c0), and one more with `player2` = -1
  /// when the Allied victory box changed. Its execution (0x004e5f80) on
  /// every peer hands the word to `SetRelation` (0x005652f0) for the two
  /// records, or stores the flag at the issuer's record `+0x64`, which the
  /// description serialiser names `allied` (0x005648bb): `playerdata/@allied`.
  diplomacy,
  /// A click on a queued unit in the info bar's `BuildingQueue`: take the
  /// command `command_id` out of `target.object`'s queue, and refund it.
  /// The strip never touches the queue itself: its click (0x006bf7c0) builds
  /// a `CVXCmdCancelCmd` of two fields -- `target`, the building, and
  /// `cmdid`, the queued command's id (their names are the serialiser's,
  /// 0x004e5680) -- and posts it through the local command path (0x0051d3c0),
  /// and its execution (0x004e63c0) on every peer is
  /// `CommandSystem::cancel_command`.
  cancel_command,
};

/// Whether a peer may send `kind` in a turn packet.
[[nodiscard]] constexpr bool is_wire_kind(NetOrderKind kind) noexcept {
  return kind == NetOrderKind::default_order || kind == NetOrderKind::command ||
         kind == NetOrderKind::surrender || kind == NetOrderKind::set_speed ||
         kind == NetOrderKind::diplomacy || kind == NetOrderKind::cancel_command;
}

/// The speeds `CVXCmdSetSpeed` accepts, per mille (0x004e67c0 clamps to them).
inline constexpr std::int32_t kMinGameSpeed = 1;
inline constexpr std::int32_t kMaxGameSpeed = 100000;
[[nodiscard]] constexpr std::int32_t clamp_game_speed(std::int32_t speed) noexcept {
  return speed < kMinGameSpeed ? kMinGameSpeed : speed > kMaxGameSpeed ? kMaxGameSpeed : speed;
}

/// One player order, as a peer receives it.
struct NetOrder {
  /// The issuing player. Not the actors' owner: `GROUP_UNITSOUT.VS` filters
  /// the actors by `DiplGetShareControl(u.player, player)` and so does
  /// `issue_default_order`, so an order may legitimately command units the
  /// issuer does not own.
  PlayerId issuer = 0;
  /// Per-issuer, ascending. Two orders from one player on one turn keep the
  /// order that player issued them in; this is what records it, because
  /// nothing else in the tuple can.
  std::uint32_t sequence = 0;
  /// The selection the order applies to, in list order.
  std::vector<ObjectId> actors;
  OrderTarget target{};
  /// Shift at the click: append rather than replace. For a `command` order
  /// too -- the bar's replace flag is the same key (0x005e39f0), and the row
  /// decides past it (a train row never replaces).
  OrderMode mode = OrderMode::replace;
  /// Ctrl at the click: `bModifier`, for a right click (0x005e6127) and a
  /// command bar row (0x005e3a13) alike. Not Shift: the two are separate
  /// flags in the original's order (`+0xc`, `+0x10`) and on its wire
  /// (0x004e7a27 packs them as two bits).
  bool modifier = false;
  NetOrderKind kind = NetOrderKind::default_order;
  /// `command` only: the row's name, as `CommandBar::press` takes it.
  std::string command;
  /// `command` only: aimed at `target`, rather than pressed.
  bool aimed = false;
  /// `command` only: how many times the row is issued. 1, except for Ctrl on
  /// a train row, where the issuing peer's bar put `TrainMultipleCount` here
  /// (`CommandBar::Flags::repeat`): one order with a count, as the original
  /// posts it, not that many orders.
  std::uint8_t repeat = 1;
  /// `set_speed` only: per mille, as asked; clamped where it is applied.
  std::int32_t speed = 0;
  /// `diplomacy` only: the player whose cell of `issuer`'s row is written,
  /// or `kNoPlayer` for `issuer`'s allied-victory flag.
  PlayerId other = kNoPlayer;
  /// `diplomacy` with a player: the whole word, not one bit.
  std::uint32_t relations = 0;
  /// `diplomacy` with `kNoPlayer`: the flag.
  bool allied = false;
  /// `cancel_command` only: the queued command's id (`cmdid`), on
  /// `target.object`.
  std::uint32_t command_id = 0;
};

/// Where the kinds other than a right click go.
///
/// A right click needs only the world and its command table; a command bar
/// row runs through the session's `CommandBar` and a surrender ends a match
/// the session keeps. The core's lockstep layer knows neither, so a caller
/// that can apply them hands this in -- `CommandBarSink` in `sim/cmdbar.hpp` is
/// the one a session uses. Without one those orders are counted `unapplied`,
/// the same on every peer, which is what keeps a harness with no session
/// honest rather than divergent.
///
/// **An order's actors are the issuer's claim, not a fact.** Implementations
/// keep only those `is_commandable(world, id, issuer)` allows, exactly as
/// `issue_default_order` does, so a peer cannot command another player's
/// army by naming it.
class NetCommandSink {
 public:
  virtual ~NetCommandSink() = default;
  /// True when the row was found and issued.
  virtual bool command(const NetOrder& order) = 0;
  virtual bool surrender(PlayerId issuer) = 0;
  /// `departed`'s seat passes to the computer. False where nothing could
  /// start an AI, which leaves the seat idle -- the same on every peer.
  virtual bool take_over(PlayerId departed) {
    (void)departed;
    return false;
  }
  /// `joined`'s seat passes back from the computer to a player: its AI
  /// stops. False where nothing could stop one -- the same on every peer.
  virtual bool hand_back(PlayerId joined) {
    (void)joined;
    return false;
  }
};

/// The orders for one turn, as they arrived: unsorted.
struct NetTurn {
  std::vector<NetOrder> orders;

  [[nodiscard]] bool empty() const noexcept { return orders.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return orders.size(); }
};

/// Where the local player's posted orders go: the original's local command
/// path (0x0051d3c0), which every `CVXCmd*` a script or a screen builds is
/// posted through. `HostContext::outbox` points at one.
///
/// An interface because the answer differs by embedder and the core must not
/// know which: a networked peer queues the order for an agreed turn
/// (`engine/app/netplay.hpp`), a single-player session runs it on its next
/// turn (`LocalOrders` below), a headless run has none.
class OrderOutbox {
 public:
  virtual ~OrderOutbox() = default;
  /// Game `+0x230`: the match fixed its speed, and `SetSpeed` posts nothing
  /// (0x004c6ce0 tests it first). Set at the start of a match from the
  /// settings' `gamespeed` (0x005268a1): -1 is variable, anything else fixes
  /// it.
  [[nodiscard]] virtual bool speed_fixed() const noexcept { return false; }
  /// `order.issuer` is the poster's claim; the embedder may overwrite it
  /// with the seat it knows, as a networked peer does.
  virtual void post(NetOrder order) = 0;
};

/// A single-player session's outbox: posted orders wait for the next turn,
/// which `take` hands over in the order they were posted.
///
/// **The turn they run on, read**: a command posted while turn *t* runs, or
/// between turns, is executed by the pump at the head of the next turn, and
/// the clock (0x00528a80) converts that turn's window only after the pump
/// has run -- which is what `TurnNegotiator::take` also does, converting turn
/// *t*'s length before *t*'s orders are applied. So one rule serves both: the
/// length is converted first, the orders applied second, and a speed takes
/// effect from the turn after the one it was applied on.
class LocalOrders final : public OrderOutbox {
 public:
  [[nodiscard]] bool speed_fixed() const noexcept override { return speed_fixed_; }
  void set_speed_fixed(bool fixed) noexcept { speed_fixed_ = fixed; }
  /// Numbers the order by its place among those waiting, so the canonical
  /// order of a turn is the order they were posted in.
  void post(NetOrder order) override;
  /// Every order posted since the last call, as one turn's.
  [[nodiscard]] NetTurn take();
  [[nodiscard]] bool empty() const noexcept { return pending_.orders.empty(); }

 private:
  NetTurn pending_;
  bool speed_fixed_ = false;
};

/// A whole match's orders, one entry per turn.
///
/// Turn `i` of a schedule takes `turns[i - first]`; a stream shorter than the
/// schedule simply runs out, which is what a match with a quiet tail looks
/// like.
///
/// `first` is zero except on a peer that joined a running match
/// (`sim/netjoin.hpp`): it never saw the turns before the one it joined at,
/// so its stream starts there, and `prior` is the stream hash through them,
/// as the host that admitted it computed it. What follows folds on from that
/// value, which is what lets a joiner's `netcmds` equal everyone else's from
/// its first turn.
struct CommandStream {
  std::vector<NetTurn> turns;
  std::uint32_t first = 0;
  /// `stream_hash` through turn `first`; unread while `first` is zero.
  std::uint64_t prior = 0;

  [[nodiscard]] const NetTurn* at(std::size_t turn) const noexcept {
    return turn >= first && turn - first < turns.size() ? &turns[turn - first] : nullptr;
  }
  /// The turn after the last one held: `first` plus how many are.
  [[nodiscard]] std::size_t size() const noexcept { return first + turns.size(); }
  /// Orders over the whole stream, for a report that says how much was driven.
  [[nodiscard]] std::size_t order_count() const noexcept;
};

/// The turn's orders in the sequence every peer applies them in: by `issuer`,
/// then by `sequence`, then by the position the order arrived at.
///
/// The third key is what makes the rule total rather than merely
/// deterministic-looking: two orders from one player carrying the same
/// sequence number are a caller's bug, and a sort that left them in arrival
/// order would make that bug a desync instead of a repeatable result.
[[nodiscard]] std::vector<const NetOrder*> canonical_order(const NetTurn& turn);

/// Apply one turn's orders to a world, in canonical order.
///
/// Returns the orders that reached `issue_default_order`. An order whose
/// actors are all gone is still applied -- it resolves to nothing and reports
/// refusals -- because skipping it on one peer and not the other is exactly
/// the divergence this exists to prevent.
///
/// A world with no `CommandSystem` refuses every order, which
/// `issue_default_order` already does; this reports it as zero applied rather
/// than as an error, for the same reason.
struct NetTurnReport {
  std::size_t applied = 0;     ///< orders handed to `issue_default_order`
  std::size_t issued = 0;      ///< commands queued, summed over those orders
  std::size_t refused = 0;     ///< actors skipped: gone, or not the issuer's to command
  std::size_t unresolved = 0;  ///< actors whose class offered no verb for the target
  std::size_t blocked = 0;     ///< actors whose candidate would not verify
  std::size_t commands = 0;    ///< command rows and surrenders the sink carried out
  std::size_t unapplied = 0;   ///< command rows and surrenders nothing carried out
  std::size_t speeds = 0;      ///< `set_speed` orders written to the clock
  std::size_t diplomacy = 0;   ///< `diplomacy` orders written to the players
  std::size_t cancels = 0;     ///< `cancel_command` orders that took a command out
};
NetTurnReport apply_turn(World& world, const NetTurn& turn, OrderVerifier* verifier = nullptr,
                         NetCommandSink* sink = nullptr);

/// The head of one unnetworked turn: `real_ms` converted at the clock's speed
/// **first**, then whatever `orders` holds applied. The caller advances the
/// world by `length`. The order of the two is the rule `LocalOrders`
/// describes, and it is here rather than in the app so that it is tested.
struct LocalTurn {
  std::int32_t length = 0;  ///< game-time units, at the speed before `orders`
  NetTurn orders;           ///< what was applied, in posting order
  NetTurnReport report;
};
[[nodiscard]] LocalTurn begin_local_turn(World& world, LocalOrders& orders, std::int32_t real_ms,
                                         OrderVerifier* verifier = nullptr,
                                         NetCommandSink* sink = nullptr);

/// The stream's hash after `turns` turns have been applied.
///
/// Folds each applied turn's orders **in canonical order**, so that two peers
/// handed the same orders in different arrival orders agree, and two peers
/// handed different orders do not. Not the original's fold; see the header.
///
/// On a stream that starts at `first`, the fold resumes from `prior`; asked
/// for fewer turns than `first`, it can only answer `prior`.
[[nodiscard]] std::uint64_t stream_hash(const CommandStream& stream, std::size_t turns) noexcept;

/// The fold of one order, exposed so a test can pin which fields reach it.
///
/// Every field of `NetOrder` is folded, `actors` in list order. A field left
/// out would be a field two peers could disagree about in silence. The three
/// `diplomacy` fields are folded for that kind only, so a stream without one
/// hashes as it did before they existed.
void hash_order(std::uint64_t& state, const NetOrder& order) noexcept;

/// A `conformance::Driver` that plays a `CommandStream` into a peer.
///
/// Holds the stream by reference and nothing else -- no cursor, no per-peer
/// state -- because `conformance::Driver` requires exactly that: the harness
/// drives two peers from one driver and a cursor would make the second peer
/// see a different match from the first.
class StreamDriver final : public conformance::Driver {
 public:
  explicit StreamDriver(const CommandStream& stream, OrderVerifier* verifier = nullptr) noexcept
      : stream_(&stream), verifier_(verifier) {}

  [[nodiscard]] std::uint64_t drive(conformance::Run& run, std::size_t turn) override {
    if (const NetTurn* orders = stream_->at(turn); orders != nullptr) {
      const NetTurnReport report = apply_turn(run.world(), *orders, verifier_);
      applied_ += report.applied;
      issued_ += report.issued;
      refused_ += report.refused;
      unresolved_ += report.unresolved;
      blocked_ += report.blocked;
    }
    // Through and including this turn, so that a peer which was handed a
    // different turn `n` says so on turn `n` rather than on the turn its
    // consequences happen to become visible.
    return stream_hash(*stream_, turn + 1);
  }

  /// Over every peer driven, so two peers over one stream double these. What
  /// they are for is a report that can say the match was not empty -- a
  /// lockstep check that drove no orders passes for the wrong reason, and
  /// this is what lets a caller refuse to believe it.
  [[nodiscard]] std::size_t applied() const noexcept { return applied_; }
  [[nodiscard]] std::size_t issued() const noexcept { return issued_; }
  [[nodiscard]] std::size_t refused() const noexcept { return refused_; }
  [[nodiscard]] std::size_t unresolved() const noexcept { return unresolved_; }
  [[nodiscard]] std::size_t blocked() const noexcept { return blocked_; }

 private:
  const CommandStream* stream_;
  OrderVerifier* verifier_ = nullptr;
  std::size_t applied_ = 0;
  std::size_t issued_ = 0;
  std::size_t refused_ = 0;
  std::size_t unresolved_ = 0;
  std::size_t blocked_ = 0;
};

}  // namespace imperivm::core::sim
