#include "imperivm/core/sim/netdepart.hpp"

#include <algorithm>

namespace imperivm::core::sim {

const char* describe(DepartureOutcome::State state) noexcept {
  switch (state) {
    case DepartureOutcome::State::playing: return "playing";
    case DepartureOutcome::State::coordinator_left: return "the host left";
    case DepartureOutcome::State::dropped: return "the other players dropped this one";
    case DepartureOutcome::State::irreconcilable: return "the players disagree about a departure";
  }
  return "?";
}

DepartureOutcome settle_departures(TurnNegotiator& negotiator, LinkNode& node) {
  DepartureOutcome out;
  const PlayerId self = negotiator.local();
  const PlayerId coordinator = negotiator.config().resolved_coordinator();
  const std::vector<PlayerId> gone = node.departed();
  const auto is_gone = [&gone](PlayerId peer) {
    return std::binary_search(gone.begin(), gone.end(), peer);
  };
  // Each peer's current stint only: a seat a late joiner took is a new part,
  // and what was decided about the one before it is not about this one.
  const std::map<PlayerId, std::uint32_t> drops = node.drops();
  const std::map<std::pair<PlayerId, PlayerId>, std::uint32_t> reports = node.reports();

  if (drops.count(self) != 0) {
    out.state = DepartureOutcome::State::dropped;
    return out;
  }
  if (coordinator != self && is_gone(coordinator)) {
    out.state = DepartureOutcome::State::coordinator_left;
    return out;
  }

  // 1. Freeze and report, once per departed peer this one has not heard the
  //    decision on. A peer that learns the decision first never ran past it:
  //    the coordinator waited for its report, so it cannot be one of those.
  for (const PlayerId departed : gone) {
    if (departed == self || drops.count(departed) != 0) continue;
    if (reports.count({departed, self}) != 0) continue;
    node.report(departed, negotiator.suspend(departed));
    out.news = true;
  }

  // 2. Decide, as the coordinator, once every peer not known to have gone has
  //    reported. The largest bound: see the header.
  if (self == coordinator) {
    const std::map<std::pair<PlayerId, PlayerId>, std::uint32_t> heard = node.reports();
    for (const PlayerId departed : gone) {
      if (departed == self || drops.count(departed) != 0) continue;
      bool complete = true;
      std::uint32_t largest = 0;
      for (const PlayerId peer : negotiator.config().peers) {
        if (peer == departed || is_gone(peer)) continue;
        const auto report = heard.find({departed, peer});
        if (report == heard.end()) {
          complete = false;
          break;
        }
        largest = std::max(largest, report->second);
      }
      if (!complete) continue;
      node.decide(departed, largest);
      out.news = true;
    }
  }

  // 3. Apply every decision, and every admission, in turn order.
  const MembershipOutcome applied = apply_membership(negotiator, node);
  out.applied = applied.drops;
  if (!applied.ok) out.state = DepartureOutcome::State::irreconcilable;
  return out;
}

Delivery deliver(TurnNegotiator& negotiator, LinkNode& node, std::span<const std::byte> datagram,
                 std::uint32_t now) {
  Delivery out;
  out.receipt = node.receive(datagram, now);
  out.late = out.receipt.late;
  // A late joiner's first packets come with its admission: the negotiator
  // must know the seat is played again before it is handed them.
  out.irreconcilable = !apply_membership(negotiator, node).ok;
  for (const TurnPacket& packet : out.receipt.packets) {
    const ReceiveStatus status = negotiator.receive(packet);
    if (status == ReceiveStatus::accepted) {
      ++out.accepted;
    } else if (status == ReceiveStatus::departed) {
      ++out.late;
    } else {
      ++out.refused;
    }
  }
  return out;
}

MembershipOutcome apply_membership(TurnNegotiator& negotiator, LinkNode& node) {
  MembershipOutcome out;
  const PlayerId self = negotiator.local();
  // Per seat, in the order its stints happened: a stint's end before the
  // next stint's start, which the negotiator requires.
  struct Event {
    PlayerId peer;
    std::uint32_t turn;
    bool admit;
    std::uint32_t since;
  };
  std::vector<Event> events;
  for (const Drop& drop : node.decisions()) {
    events.push_back(Event{drop.departed, drop.from_turn, false, drop.since});
  }
  for (const Admit& admit : node.admits()) {
    events.push_back(Event{admit.peer, admit.from_turn, true, admit.from_turn});
  }
  // A stint's admission before its end: the end is about the stint, which
  // must exist first. (The other way round was the first version, and a
  // fault injected here -- swapping it -- survived, because no test had a
  // joiner that came and went between two datagrams. One does now.)
  std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    if (a.peer != b.peer) return a.peer < b.peer;
    return a.since != b.since ? a.since < b.since : a.admit && !b.admit;
  });
  for (const Event& event : events) {
    if (event.admit) {
      // Its own admission is how a joiner was built; nothing to apply.
      if (event.peer == self) continue;
      const std::vector<SeatStints> membership = negotiator.membership();
      bool known = false;
      for (const SeatStints& seat : membership) {
        if (seat.peer != event.peer) continue;
        for (const Stint& stint : seat.stints) known = known || stint.from == event.turn;
      }
      if (known) continue;
      if (!negotiator.admit(event.peer, event.turn)) {
        out.ok = false;
        return out;
      }
      out.admits.push_back(Admit{event.peer, event.turn});
      continue;
    }
    if (event.peer == self) continue;
    const std::vector<SeatStints> membership = negotiator.membership();
    bool applied = false;
    for (const SeatStints& seat : membership) {
      if (seat.peer != event.peer) continue;
      for (const Stint& stint : seat.stints) {
        applied = applied || (stint.from == event.since && stint.until.has_value());
      }
    }
    // Already applied: only checked, which a different end fails.
    const bool agreed = negotiator.drop(event.peer, event.turn, event.since);
    if (applied && agreed) continue;
    if (!agreed) {
      out.ok = false;
      return out;
    }
    out.drops.push_back(Drop{event.peer, event.turn, event.since});
  }
  return out;
}

ComputerSeats ComputerSeats::resumed(std::span<const SeatStints> membership, std::uint32_t turn,
                                     PlayerId local) {
  ComputerSeats out(local);
  for (const SeatStints& seat : membership) {
    if (seat.peer == local || seat.peer >= kPlayerCount || seat.stints.empty()) continue;
    const bool plays = std::any_of(seat.stints.begin(), seat.stints.end(), [turn](const Stint& stint) {
      return stint.from <= turn && (!stint.until.has_value() || turn < *stint.until);
    });
    // Left before `turn`, rather than not yet begun: a seat whose only stint
    // opens after it is nobody's departure.
    const bool left = std::any_of(seat.stints.begin(), seat.stints.end(), [turn](const Stint& stint) {
      return stint.until.has_value() && *stint.until <= turn;
    });
    if (!plays && left) out.marked_[seat.peer] = true;
  }
  return out;
}

ComputerSeats::Change ComputerSeats::apply(const NetTurn& turn) {
  Change out;
  for (const NetOrder& order : turn.orders) {
    const PlayerId seat = order.issuer;
    if (seat == local_ || seat >= kPlayerCount) continue;
    if (order.kind == NetOrderKind::departed && !marked_[seat]) {
      marked_[seat] = true;
      out.taken.push_back(seat);
    } else if (order.kind == NetOrderKind::joined && marked_[seat]) {
      marked_[seat] = false;
      out.handed_back.push_back(seat);
    }
  }
  return out;
}

std::string ComputerSeats::display(PlayerId seat, std::string_view name, std::string_view marker) const {
  std::string out(name);
  if (marked(seat)) out.append(marker);
  return out;
}

}  // namespace imperivm::core::sim
