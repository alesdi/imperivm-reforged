#include "imperivm/core/sim/netcmds.hpp"

#include <algorithm>
#include <utility>

namespace imperivm::core::sim {
namespace {

/// FNV-1a, 64 bit -- the same construction `world.cpp` folds the state vector
/// with, so a `netcmds` value and a `slots` value are read the same way. It is
/// duplicated here rather than shared for the reason `conformance.cpp` gives
/// for its own copy: the alternative is exporting the world's hashing, and a
/// hash that other code can reach is a hash other code can change.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void fold(std::uint64_t& state, std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    state ^= (value >> shift) & 0xFF;
    state *= kFnvPrime;
  }
}

void fold_i32(std::uint64_t& state, std::int32_t value) noexcept {
  fold(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

}  // namespace

void LocalOrders::post(NetOrder order) {
  order.sequence = static_cast<std::uint32_t>(pending_.orders.size());
  pending_.orders.push_back(std::move(order));
}

NetTurn LocalOrders::take() {
  NetTurn turn = std::move(pending_);
  pending_.orders.clear();
  return turn;
}

std::size_t CommandStream::order_count() const noexcept {
  std::size_t total = 0;
  for (const NetTurn& turn : turns) total += turn.orders.size();
  return total;
}

std::vector<const NetOrder*> canonical_order(const NetTurn& turn) {
  std::vector<const NetOrder*> sorted;
  sorted.reserve(turn.orders.size());
  for (const NetOrder& order : turn.orders) sorted.push_back(&order);
  // `stable_sort` on the two declared keys, with arrival order as the third:
  // the stability *is* the third key, so two orders a caller gave the same
  // (issuer, sequence) come out in the order they were handed over rather
  // than in whichever order the sort happened to leave them.
  std::stable_sort(sorted.begin(), sorted.end(), [](const NetOrder* a, const NetOrder* b) {
    if (a->issuer != b->issuer) return a->issuer < b->issuer;
    return a->sequence < b->sequence;
  });
  return sorted;
}

void hash_order(std::uint64_t& state, const NetOrder& order) noexcept {
  fold(state, order.issuer);
  fold(state, order.sequence);
  fold(state, order.actors.size());
  for (const ObjectId actor : order.actors) fold(state, actor);
  fold_i32(state, order.target.point.x);
  fold_i32(state, order.target.point.y);
  fold(state, order.target.object);
  fold(state, static_cast<std::uint64_t>(order.mode));
  fold(state, order.modifier ? 1u : 0u);
  fold(state, static_cast<std::uint64_t>(order.kind));
  fold(state, order.aimed ? 1u : 0u);
  fold(state, order.command.size());
  for (const char c : order.command) fold(state, static_cast<std::uint8_t>(c));
  fold_i32(state, order.speed);
  // A command's count only where it is not the one every order had before
  // it existed, for the reason the diplomacy fields below are folded only
  // where they mean something.
  if (order.kind == NetOrderKind::command && order.repeat != 1) fold(state, 0x100u + order.repeat);
  // The diplomacy fields only where they mean something: folding them for
  // every kind would move the hash of every stream recorded before them.
  if (order.kind == NetOrderKind::cancel_command) fold(state, order.command_id);
  if (order.kind == NetOrderKind::diplomacy) {
    fold(state, order.other);
    fold(state, order.relations);
    fold(state, order.allied ? 1u : 0u);
  }
}

std::uint64_t stream_hash(const CommandStream& stream, std::size_t turns) noexcept {
  // A joiner's stream resumes from the host's value at the turn it joined.
  std::uint64_t state = stream.first == 0 ? kFnvOffset : stream.prior;
  const std::size_t last = std::min(turns, stream.size());
  for (std::size_t turn = stream.first; turn < last; ++turn) {
    // The turn index is folded even for an empty turn: a stream that is quiet
    // for three turns and one that is quiet for four are different streams,
    // and a fold over the orders alone could not tell them apart. It is the
    // match's turn, not the position in this stream, so a joiner's fold is
    // the same fold.
    fold(state, turn);
    const std::vector<const NetOrder*> sorted = canonical_order(*stream.at(turn));
    fold(state, sorted.size());
    for (const NetOrder* order : sorted) hash_order(state, *order);
  }
  return state;
}

LocalTurn begin_local_turn(World& world, LocalOrders& orders, std::int32_t real_ms,
                           OrderVerifier* verifier, NetCommandSink* sink) {
  LocalTurn turn;
  turn.length = turn_length_from_real_ms(real_ms, world.clock().config().game_speed);
  turn.orders = orders.take();
  if (!turn.orders.empty()) turn.report = apply_turn(world, turn.orders, verifier, sink);
  return turn;
}

NetTurnReport apply_turn(World& world, const NetTurn& turn, OrderVerifier* verifier,
                         NetCommandSink* sink) {
  NetTurnReport report;
  const CommandTable* commands = order_command_table(world);
  for (const NetOrder* order : canonical_order(turn)) {
    if (order->kind == NetOrderKind::set_speed) {
      // The world's clock alone, and on every peer: no sink is needed, so an
      // unnetworked replay of the stream does it too. The clock's speed is
      // the rate the *next* turn's length is converted at; the negotiator
      // converts with it from the next turn (`TurnNegotiator::take`).
      world.clock().set_game_speed(clamp_game_speed(order->speed));
      ++report.speeds;
      continue;
    }
    if (order->kind == NetOrderKind::diplomacy) {
      // The players' table alone, on every peer, like the speed: no sink.
      // 0x004e5f80: with a second player, the word goes to `SetRelation`,
      // which returns at once for a player's own record; without one, the
      // issuer's `allied` flag. `DiplShareView`'s exploration merge
      // (0x00564fa0, which this execution also calls when bit 4 is set) is
      // not done, for the reason `player_host.cpp` gives for the script's.
      PlayerTable& players = world.players();
      if (PlayerTable::is_valid(order->issuer)) {
        if (order->other == kNoPlayer) {
          players.setup(order->issuer).allied_flag = order->allied;
          ++report.diplomacy;
        } else if (PlayerTable::is_valid(order->other) && order->other != order->issuer) {
          players.set_relation_word(order->issuer, order->other, order->relations);
          ++report.diplomacy;
        }
      }
      continue;
    }
    if (order->kind == NetOrderKind::cancel_command) {
      // The world's queues alone, on every peer: no sink. 0x004e63c0 checks
      // nothing about who asked; the strip that posts it does (0x006bfd00:
      // the building's owner grants the issuer shared control), and that is
      // `is_commandable`'s test, applied here too -- this engine's, as every
      // order's filter is -- so that a peer cannot empty another player's
      // barracks by naming it.
      CommandSystem* queues = command_system(world);
      if (queues != nullptr && is_commandable(world, order->target.object, order->issuer) &&
          queues->cancel_command(world, order->target.object, order->command_id)) {
        ++report.cancels;
      } else {
        ++report.unapplied;
      }
      continue;
    }
    if (order->kind != NetOrderKind::default_order) {
      // In canonical order with the right clicks, not before or after them:
      // a player who clicked and then pressed a row did it in that order.
      bool done = false;
      if (sink != nullptr) {
        switch (order->kind) {
          case NetOrderKind::command: done = sink->command(*order); break;
          case NetOrderKind::surrender: done = sink->surrender(order->issuer); break;
          case NetOrderKind::departed: done = sink->take_over(order->issuer); break;
          case NetOrderKind::joined: done = sink->hand_back(order->issuer); break;
          case NetOrderKind::default_order:
          case NetOrderKind::set_speed:
          case NetOrderKind::diplomacy:
          case NetOrderKind::cancel_command: break;
        }
      }
      ++(done ? report.commands : report.unapplied);
      continue;
    }
    if (commands == nullptr) continue;
    const OrderReport issued = issue_default_order(world, *commands, order->actors, order->target,
                                                   order->mode, order->modifier, order->issuer,
                                                   verifier);
    ++report.applied;
    report.issued += issued.issued;
    report.refused += issued.refused;
    report.unresolved += issued.unresolved;
    report.blocked += issued.blocked;
  }
  return report;
}

}  // namespace imperivm::core::sim
