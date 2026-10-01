#pragma once

/// A peer that leaves mid-match, and how the rest carry on without it.
///
/// Lockstep runs a turn only when every peer's packet for it is in hand, so a
/// peer that leaves stalls everyone on the first turn it never sent. Carrying
/// on means removing it -- and removing it at *different turns* on different
/// peers is the desync this file exists to prevent: a peer that ran turn `t`
/// with the departed player's orders and a peer that ran it without them are
/// playing two games from then on. So the remaining peers must agree on one
/// number, **the turn the departed peer's part ends at**, and every one of
/// them must apply exactly its packets below that turn and none from it on.
///
/// ## What the original does, as read from `gbr.exe`
///
/// It never ends the match for everyone. A player that quits posts a
/// `CVXPlayerWantsToQuit` command into the command stream (0x006b47f6), agreed
/// on a turn like any order; a player that goes silent is timed out -- 90 s
/// while the stuck tick is at most 5, 30 s after (0x004083ef) -- and the split
/// (0x00408520) drops every seat with no data for the stuck tick `t`. So its
/// last turn is agreed only implicitly, as the tick every survivor happens to
/// be stuck on, and a survivor holding the missing player's packet for `t`
/// can diverge from one that does not. Either way the drop routine
/// (0x00406840) hands each dropped human seat to the computer.
///
/// ## The agreement, which is this engine's
///
/// Explicit rather than implicit, because the link layer here relays every
/// packet and acknowledges each sender's packets up to a known turn, which is
/// exactly the fact the agreement needs. The rule:
///
///   1. **Freeze and report.** A peer that learns `D` has gone -- a refusal
///      from `D`, silence the platform timed, or any other peer's report --
///      stops running turns past what it already holds of `D`'s
///      (`TurnNegotiator::suspend`) and reports that bound: "I hold `D`'s
///      packets for every turn below `N`". Every turn it has run is below `N`,
///      and so is every turn it runs until the decision.
///   2. **One peer decides.** The coordinator (`LockstepConfig::coordinator`,
///      the host) waits for a report from every peer not known to have gone,
///      and decides `L`, the **largest** reported bound. The largest is the
///      only choice that can be safe: a smaller one would cut a turn somebody
///      has already run with `D`'s orders in it. And it is always available:
///      the peer that reported it holds every one of `D`'s packets below it,
///      and the link layer carries them to the rest.
///   3. **Everyone applies it** (`TurnNegotiator::drop`), waiting for `D`'s
///      packets below `L` and taking none from `L` on.
///
/// The reports and the decision ride in every datagram (`sim/netlink.hpp`), so
/// loss repairs them and a relay passes them on without understanding them.
///
/// **The coordinator itself leaving ends the match** for everyone, visibly:
/// there is nobody left to decide, and in the lobby's topology the host is
/// also the relay every joiner links to, so nothing could have been carried
/// on anyway. That too is this engine's rule, and the one place it does less
/// than the original, whose peers are a mesh.
///
/// ## The computer takes the departed seat on turn `L`
///
/// **The takeover is the original's**: 0x00406840 gives each dropped seat that
/// is human, was present at the start, has no AI yet and is not the local
/// seat to the computer through 0x00434ca0 -- the body behind `AIStart` --
/// with no profile. (In a rated GameSpy game it calls another routine,
/// 0x00524090, which is inferred to declare defeat; the online service is gone
/// and that path is not built.) **Where it happens is this engine's**: the
/// negotiator writes a `NetOrderKind::departed` order for `D` into turn `L`,
/// the first turn without `D`'s packet, so the takeover is part of the agreed
/// history -- in the stream hash, in every replay of it, and on the same turn
/// on every peer by construction. `NetCommandSink::take_over` starts the AI;
/// a caller with no session to start one in counts it unapplied, the same on
/// every peer, and the seat stays idle there.
///
/// ## What the players are shown
///
/// **The original's**: for each seat it hands over, 0x00406840 prints
/// `Player %s1 dropped` -- translated, with the player's name -- then appends
/// the translation of `(AI)` (context empty; `TRANSLATION.LOC.XML` carries it,
/// commented "Added to names of AI players") to the name in the player record,
/// verbatim, with no separator. After the loop, if it handed over any seat, it
/// plays `Sounds/UI/PlayerDropped.wav` **once**, however many seats went. That
/// routine is the only reader of the `(AI)` string, and nothing takes the
/// marker off again: nothing in the original ever gives a seat back.
///
/// **Where it lives is this engine's**: the marker is presentation, so it is
/// never written into `PlayerSetup::name` -- it is in no hash and no save.
/// `ComputerSeats` holds it beside the match, driven by the agreed turns'
/// `departed` and `joined` orders, so every peer marks the same seat on the
/// same turn it applies the takeover. A late join (`sim/netjoin.hpp`, which
/// has no original counterpart) is the takeover's inverse, and so is its
/// presentation: the marker goes when the joiner's first turn hands the seat
/// back, silently -- the original has no sound for a return because it has no
/// return.

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netlink.hpp"

namespace imperivm::core::sim {

struct DepartureOutcome {
  enum class State : std::uint8_t {
    playing,
    /// The coordinator has gone: no departure can be decided.
    coordinator_left,
    /// The coordinator dropped this peer: the others carry on without it.
    dropped,
    /// A decision this peer cannot apply -- it already ran a turn the decision
    /// cuts. Never on an honest match; reported rather than ignored.
    irreconcilable,
  };
  State state = State::playing;
  /// Drops applied to the negotiator by this call, in departed-peer order.
  std::vector<Drop> applied;
  /// This call reported or decided something the link layer should send.
  bool news = false;
};

[[nodiscard]] const char* describe(DepartureOutcome::State state) noexcept;

/// Everything a peer does about departures, from what its negotiator and its
/// link node hold: report every departure it knows of and has not reported,
/// decide every one it can if it is the coordinator, and apply every decision
/// -- and every late joiner's admission -- it has not applied. Idempotent --
/// call it after anything that could have changed what the node knows (a
/// datagram received, a departure seen), and a call with nothing new does
/// nothing.
DepartureOutcome settle_departures(TurnNegotiator& negotiator, LinkNode& node);

/// What `apply_membership` changed.
struct MembershipOutcome {
  /// False when the negotiator refused a decision or an admission: it has
  /// already run a turn the fact would have changed. Never on an honest match.
  bool ok = true;
  std::vector<Drop> drops;
  std::vector<Admit> admits;
};

/// Apply to the negotiator every drop and admission the node knows and the
/// negotiator does not, each seat's in the order they happened. The third
/// step of `settle_departures`, and also what a transport calls **between
/// receiving a datagram and handing its packets to the negotiator**: a
/// datagram that brings a late joiner's first packets also brings its
/// admission, and the negotiator must know the seat is played again before it
/// is handed a packet for it.
MembershipOutcome apply_membership(TurnNegotiator& negotiator, LinkNode& node);

/// What `deliver` did with one datagram.
struct Delivery {
  LinkReceipt receipt;
  std::size_t accepted = 0;
  /// A departed peer's packets past its end, the node's or the negotiator's
  /// count: sent before it left, not refused.
  std::size_t late = 0;
  /// The negotiator refused a packet the link layer delivered: the two layers
  /// disagree, which never happens on an honest match.
  std::size_t refused = 0;
  /// `apply_membership` failed.
  bool irreconcilable = false;
};

/// One datagram, the way every transport must take it: the link layer, then
/// the membership facts it brought, then its new packets to the negotiator.
/// `check_netplay` and `NetMatch` both take datagrams through this.
Delivery deliver(TurnNegotiator& negotiator, LinkNode& node, std::span<const std::byte> datagram,
                 std::uint32_t now);

/// Which seats the computer took from a departed player, for the screen to
/// mark: the "(AI)" rename (see *What the players are shown* above).
/// Presentation state beside the match, never in `World`.
class ComputerSeats {
 public:
  /// `local` is never marked, as 0x00406840 never hands over the local seat.
  explicit ComputerSeats(PlayerId local = kNoPlayer) noexcept : local_(local) {}

  /// A late joiner's marks at its first turn, from the membership it was
  /// told: every other seat that has a part in the match and plays no part in
  /// `turn` -- its last stint ended before it. A seat dropped later is marked
  /// when the joiner applies that turn's takeover, like anyone.
  [[nodiscard]] static ComputerSeats resumed(std::span<const SeatStints> membership,
                                             std::uint32_t turn, PlayerId local);

  struct Change {
    /// Seats the computer took on this turn: marked, and -- if any -- the
    /// dropped-player sound, once.
    std::vector<PlayerId> taken;
    /// Seats a late joiner took back: unmarked.
    std::vector<PlayerId> handed_back;
    [[nodiscard]] bool empty() const noexcept { return taken.empty() && handed_back.empty(); }
  };

  /// One agreed turn's orders, in the order `apply_turn` applies them: a
  /// `departed` order marks its issuer's seat, a `joined` one unmarks it. A
  /// seat already in the state an order would put it in is not reported
  /// again.
  Change apply(const NetTurn& turn);

  [[nodiscard]] bool marked(PlayerId seat) const noexcept {
    return seat < kPlayerCount && marked_[seat];
  }

  /// `name` as the screen shows it: with `marker` -- the translated `(AI)` --
  /// appended verbatim while the computer holds the seat, as the original's
  /// append does.
  [[nodiscard]] std::string display(PlayerId seat, std::string_view name,
                                    std::string_view marker) const;

 private:
  PlayerId local_;
  std::array<bool, kPlayerCount> marked_{};
};

}  // namespace imperivm::core::sim
