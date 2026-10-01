#include "imperivm/core/sim/lockstep.hpp"

#include <algorithm>
#include <map>
#include <memory>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/sim/netdepart.hpp"
#include "imperivm/core/sim/netjoin.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {
namespace {

constexpr char kMagic[] = "IMLT";

/// The smallest an order can be on the wire: a zero actor count, the point,
/// the object, two flag bytes. What bounds a count before it is believed.
constexpr std::size_t kMinOrderBytes = 4 + 4 + 4 + 4 + 1 + 1 + 1 + 1 + 1 + 4;

bool get_i32(ByteReader& reader, std::int32_t& out) noexcept {
  std::uint32_t raw = 0;
  if (!reader.u32(raw)) return false;
  out = static_cast<std::int32_t>(raw);
  return true;
}

}  // namespace

// --------------------------------------------------------------------------
// the wire
// --------------------------------------------------------------------------

std::vector<std::byte> encode(const TurnPacket& packet) {
  std::vector<std::byte> out;
  for (std::size_t i = 0; i < 4; ++i) bytes::put_u8(out, static_cast<std::uint8_t>(kMagic[i]));
  bytes::put_u8(out, kTurnPacketVersion);
  bytes::put_u8(out, packet.peer);
  bytes::put_u32(out, packet.turn);
  bytes::put_i32(out, packet.proposed_ms);
  bytes::put_u32(out, static_cast<std::uint32_t>(packet.orders.size()));
  for (const NetOrder& order : packet.orders) {
    bytes::put_u32(out, static_cast<std::uint32_t>(order.actors.size()));
    for (const ObjectId actor : order.actors) bytes::put_u32(out, actor);
    bytes::put_i32(out, order.target.point.x);
    bytes::put_i32(out, order.target.point.y);
    bytes::put_u32(out, order.target.object);
    bytes::put_u8(out, static_cast<std::uint8_t>(order.mode));
    bytes::put_u8(out, order.modifier ? 1u : 0u);
    bytes::put_u8(out, static_cast<std::uint8_t>(order.kind));
    bytes::put_u8(out, order.aimed ? 1u : 0u);
    const std::size_t length = std::min(order.command.size(), kMaxCommandName + 1);
    bytes::put_u8(out, static_cast<std::uint32_t>(length));
    for (std::size_t i = 0; i < length; ++i) {
      bytes::put_u8(out, static_cast<std::uint8_t>(order.command[i]));
    }
    bytes::put_i32(out, order.speed);
    if (order.kind == NetOrderKind::command) bytes::put_u8(out, order.repeat);
    if (order.kind == NetOrderKind::cancel_command) bytes::put_u32(out, order.command_id);
    if (order.kind == NetOrderKind::diplomacy) {
      bytes::put_u8(out, order.other);
      bytes::put_u32(out, order.relations);
      bytes::put_u8(out, order.allied ? 1u : 0u);
    }
  }
  return out;
}

const char* describe(DecodeStatus status) noexcept {
  switch (status) {
    case DecodeStatus::ok: return "ok";
    case DecodeStatus::truncated: return "truncated";
    case DecodeStatus::bad_magic: return "bad magic";
    case DecodeStatus::bad_version: return "bad version";
    case DecodeStatus::bad_field: return "field out of range";
    case DecodeStatus::trailing: return "trailing bytes";
  }
  return "?";
}

DecodeStatus decode(std::span<const std::byte> data, TurnPacket& out) {
  out = TurnPacket{};
  if (data.size() < 4) return DecodeStatus::truncated;
  if (!has_magic(data, "IMLT")) return DecodeStatus::bad_magic;
  ByteReader reader(data, 4);

  std::uint8_t version = 0;
  std::uint8_t peer = 0;
  std::uint32_t count = 0;
  if (!reader.u8(version)) return DecodeStatus::truncated;
  if (version != kTurnPacketVersion) return DecodeStatus::bad_version;
  if (!reader.u8(peer) || !reader.u32(out.turn) || !get_i32(reader, out.proposed_ms) ||
      !reader.u32(count)) {
    return DecodeStatus::truncated;
  }
  out.peer = peer;
  if (out.proposed_ms <= 0) return DecodeStatus::bad_field;
  // Believed only as far as the bytes can back it.
  if (count > reader.remaining() / kMinOrderBytes) return DecodeStatus::truncated;

  out.orders.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    NetOrder order;
    order.issuer = out.peer;
    order.sequence = index;
    std::uint32_t actors = 0;
    if (!reader.u32(actors)) return DecodeStatus::truncated;
    if (actors > reader.remaining() / 4) return DecodeStatus::truncated;
    order.actors.resize(actors);
    for (ObjectId& actor : order.actors) {
      if (!reader.u32(actor)) return DecodeStatus::truncated;
    }
    std::uint8_t mode = 0;
    std::uint8_t modifier = 0;
    if (!get_i32(reader, order.target.point.x) || !get_i32(reader, order.target.point.y) ||
        !reader.u32(order.target.object) || !reader.u8(mode) || !reader.u8(modifier)) {
      return DecodeStatus::truncated;
    }
    std::uint8_t kind = 0;
    std::uint8_t aimed = 0;
    std::uint8_t length = 0;
    if (!reader.u8(kind) || !reader.u8(aimed) || !reader.u8(length)) {
      return DecodeStatus::truncated;
    }
    std::span<const std::byte> name;
    if (!reader.bytes(length, name)) return DecodeStatus::truncated;
    if (!get_i32(reader, order.speed)) return DecodeStatus::truncated;
    // Any byte: the original's count is one, and zero issues nothing.
    if (kind == static_cast<std::uint8_t>(NetOrderKind::command) && !reader.u8(order.repeat)) {
      return DecodeStatus::truncated;
    }
    if (kind == static_cast<std::uint8_t>(NetOrderKind::cancel_command) &&
        !reader.u32(order.command_id)) {
      return DecodeStatus::truncated;
    }
    std::uint8_t allied = 0;
    if (kind == static_cast<std::uint8_t>(NetOrderKind::diplomacy)) {
      if (!reader.u8(order.other) || !reader.u32(order.relations) || !reader.u8(allied)) {
        return DecodeStatus::truncated;
      }
      // Another player's cell, or kNoPlayer for the flag: nothing else.
      if ((order.other != kNoPlayer && order.other >= kPlayerCount) || allied > 1) {
        return DecodeStatus::bad_field;
      }
      order.allied = allied != 0;
    }
    if (mode > static_cast<std::uint8_t>(OrderMode::append) || modifier > 1 ||
        kind > static_cast<std::uint8_t>(NetOrderKind::cancel_command) ||
        !is_wire_kind(static_cast<NetOrderKind>(kind)) || aimed > 1 ||
        length > kMaxCommandName) {
      return DecodeStatus::bad_field;
    }
    order.mode = static_cast<OrderMode>(mode);
    order.modifier = modifier != 0;
    order.kind = static_cast<NetOrderKind>(kind);
    order.aimed = aimed != 0;
    order.command.assign(reinterpret_cast<const char*>(name.data()), name.size());
    out.orders.push_back(std::move(order));
  }
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

// --------------------------------------------------------------------------
// the negotiator
// --------------------------------------------------------------------------

const char* describe(ReceiveStatus status) noexcept {
  switch (status) {
    case ReceiveStatus::accepted: return "accepted";
    case ReceiveStatus::duplicate: return "duplicate";
    case ReceiveStatus::conflicting: return "conflicting";
    case ReceiveStatus::unknown_peer: return "unknown peer";
    case ReceiveStatus::stale: return "stale";
    case ReceiveStatus::too_far: return "too far ahead";
    case ReceiveStatus::malformed: return "malformed";
    case ReceiveStatus::departed: return "from a departed peer";
  }
  return "?";
}

PlayerId LockstepConfig::resolved_coordinator() const noexcept {
  if (std::find(peers.begin(), peers.end(), coordinator) != peers.end()) return coordinator;
  if (peers.empty()) return kNoPlayer;
  return *std::min_element(peers.begin(), peers.end());
}

TurnNegotiator::TurnNegotiator(LockstepConfig config, PlayerId local)
    : config_(std::move(config)), local_(local) {
  std::sort(config_.peers.begin(), config_.peers.end());
  config_.peers.erase(std::unique(config_.peers.begin(), config_.peers.end()),
                      config_.peers.end());
  if (config_.min_ms <= 0) config_.min_ms = 1;
  if (config_.max_ms < config_.min_ms) config_.max_ms = config_.min_ms;
  config_.initial_ms = std::clamp(config_.initial_ms, config_.min_ms, config_.max_ms);
  next_submit_ = config_.input_delay;
  suspended_.resize(config_.peers.size());
  stints_.assign(config_.peers.size(), std::vector<Stint>{Stint{}});
}

TurnNegotiator::TurnNegotiator(LockstepConfig config, PlayerId local, const ResumePoint& resume)
    : TurnNegotiator(std::move(config), local) {
  for (const SeatStints& seat : resume.membership) {
    const std::optional<std::size_t> slot = slot_of(seat.peer);
    if (!slot.has_value() || seat.stints.empty()) continue;
    stints_[*slot] = seat.stints;
  }
  next_turn_ = resume.turn;
  history_.first = resume.turn;
  history_.prior = resume.netcmds;
  // Its own packets start where its own stint's agreed-empty turns end.
  next_submit_ = resume.turn + config_.input_delay;
}

const Stint* TurnNegotiator::stint_at(std::size_t slot, std::uint32_t turn) const noexcept {
  for (const Stint& stint : stints_[slot]) {
    if (turn >= stint.from && (!stint.until.has_value() || turn < *stint.until)) return &stint;
  }
  return nullptr;
}

bool TurnNegotiator::required_slot(std::size_t slot, std::uint32_t turn) const noexcept {
  const Stint* stint = stint_at(slot, turn);
  return stint != nullptr && turn >= stint->from + config_.input_delay;
}

bool TurnNegotiator::plays(PlayerId peer, std::uint32_t turn) const noexcept {
  const std::optional<std::size_t> slot = slot_of(peer);
  return slot.has_value() && stint_at(*slot, turn) != nullptr;
}

bool TurnNegotiator::required(PlayerId peer, std::uint32_t turn) const noexcept {
  const std::optional<std::size_t> slot = slot_of(peer);
  return slot.has_value() && required_slot(*slot, turn);
}

bool TurnNegotiator::suspended(PlayerId peer) const noexcept {
  const std::optional<std::size_t> slot = slot_of(peer);
  return slot.has_value() && suspended_[*slot].has_value();
}

std::optional<std::uint32_t> TurnNegotiator::dropped_from(PlayerId peer) const noexcept {
  const std::optional<std::size_t> slot = slot_of(peer);
  if (!slot.has_value()) return std::nullopt;
  return stints_[*slot].back().until;
}

std::uint32_t TurnNegotiator::suspend(PlayerId departed) {
  const std::optional<std::size_t> slot = slot_of(departed);
  if (!slot.has_value() || stints_[*slot].back().until.has_value()) return next_turn_;
  if (suspended_[*slot].has_value()) return *suspended_[*slot];
  // Contiguous from the next turn: a packet held past a gap is not one this
  // peer could run, so it is not reported as held. A stint's agreed-empty
  // turns are held by everyone.
  std::uint32_t held = std::max(next_turn_, stints_[*slot].back().from + config_.input_delay);
  for (;;) {
    const std::size_t offset = held - next_turn_;
    if (offset >= pending_.size() || !pending_[offset][*slot].has_value()) break;
    ++held;
  }
  suspended_[*slot] = held;
  return held;
}

bool TurnNegotiator::drop(PlayerId departed, std::uint32_t from_turn,
                          std::optional<std::uint32_t> since) {
  const std::optional<std::size_t> slot = slot_of(departed);
  if (!slot.has_value()) return false;
  if (since.has_value() && *since != stints_[*slot].back().from) {
    // A decision on a stint before the current one: already applied, or
    // this peer was told that stint's end when it joined.
    for (const Stint& old : stints_[*slot]) {
      if (old.from == *since) {
        return old.until == std::max(from_turn, old.from + config_.input_delay);
      }
    }
    return false;
  }
  Stint& stint = stints_[*slot].back();
  // The turns agreed in advance carry nobody's packet, so an end inside them
  // is an end at the first turn that could have carried one.
  const std::uint32_t end = std::max(from_turn, stint.from + config_.input_delay);
  if (stint.until.has_value()) return *stint.until == end;
  // Every turn from `first_turn` below `next_turn_` has run here, each with
  // this peer's packet once its stint needed one. A joiner never ran the
  // turns before it joined.
  if (std::max(end, history_.first) < next_turn_) return false;
  stint.until = end;
  // What it already sent past its end stays in the window, unread: `ready`
  // and `take` never look at a dropped slot from its end on, and `receive`
  // stores nothing more there. Clearing it here was tried and could not be
  // told apart from not clearing it -- fault-injected, nothing noticed --
  // so it is not done.
  return true;
}

bool TurnNegotiator::admit(PlayerId peer, std::uint32_t from_turn) {
  const std::optional<std::size_t> slot = slot_of(peer);
  if (!slot.has_value()) return false;
  std::vector<Stint>& stints = stints_[*slot];
  const Stint& last = stints.back();
  for (const Stint& stint : stints) {
    if (from_turn != 0 && stint.from == from_turn) return true;  // already admitted
  }
  if (!last.until.has_value() || from_turn <= *last.until || from_turn < next_turn_) return false;
  stints.push_back(Stint{from_turn, std::nullopt});
  suspended_[*slot].reset();
  return true;
}

std::vector<SeatStints> TurnNegotiator::membership() const {
  std::vector<SeatStints> out;
  for (std::size_t slot = 0; slot < config_.peers.size(); ++slot) {
    out.push_back(SeatStints{config_.peers[slot], stints_[slot]});
  }
  return out;
}

std::vector<TurnPacket> TurnNegotiator::held_from(std::uint32_t turn) const {
  std::vector<TurnPacket> out;
  for (std::size_t offset = 0; offset < pending_.size(); ++offset) {
    if (next_turn_ + offset < turn) continue;
    for (const std::optional<TurnPacket>& held : pending_[offset]) {
      if (held.has_value()) out.push_back(*held);
    }
  }
  return out;
}

std::optional<std::size_t> TurnNegotiator::slot_of(PlayerId peer) const noexcept {
  const auto it = std::lower_bound(config_.peers.begin(), config_.peers.end(), peer);
  if (it == config_.peers.end() || *it != peer) return std::nullopt;
  return static_cast<std::size_t>(it - config_.peers.begin());
}

std::optional<TurnPacket> TurnNegotiator::submit(std::vector<NetOrder> orders,
                                                 std::int32_t proposed_ms) {
  if (!slot_of(local_).has_value()) return std::nullopt;
  if (!required(local_, next_submit_)) return std::nullopt;
  if (next_submit_ != next_turn_ + config_.input_delay) return std::nullopt;
  TurnPacket packet;
  packet.peer = local_;
  packet.turn = next_submit_;
  packet.proposed_ms = proposed_ms > 0 ? proposed_ms : config_.initial_ms;
  packet.orders = std::move(orders);
  // A name the wire would refuse makes every other peer refuse the whole
  // packet, and a refused packet is a match that stalls for good. Dropped
  // here, where only the one order is lost.
  std::erase_if(packet.orders, [this](const NetOrder& order) {
    return order.command.size() > kMaxCommandName || !is_wire_kind(order.kind) ||
           (order.kind == NetOrderKind::set_speed && !config_.variable_speed);
  });
  for (std::size_t index = 0; index < packet.orders.size(); ++index) {
    packet.orders[index].issuer = local_;
    packet.orders[index].sequence = static_cast<std::uint32_t>(index);
  }
  if (receive(packet) != ReceiveStatus::accepted) return std::nullopt;
  ++next_submit_;
  return packet;
}

ReceiveStatus TurnNegotiator::receive(const TurnPacket& packet) {
  const std::optional<std::size_t> slot = slot_of(packet.peer);
  if (!slot.has_value()) return ReceiveStatus::unknown_peer;
  if (packet.turn < next_turn_ || packet.turn < config_.input_delay) return ReceiveStatus::stale;
  if (!plays(packet.peer, packet.turn)) return ReceiveStatus::departed;
  // The agreed-empty turns at the start of a joiner's stint, like the ones
  // at the start of the match: nobody sends a packet for them.
  if (!required_slot(*slot, packet.turn)) return ReceiveStatus::stale;
  // The furthest an honest peer can be. It cannot run a turn it has not had
  // our packet for, and ours reach `next_turn_ + input_delay`; having run
  // that one it is next to run the turn after, and schedules `input_delay`
  // beyond *that*.
  const std::uint64_t horizon =
      static_cast<std::uint64_t>(next_turn_) + 2ull * config_.input_delay + 1;
  if (packet.turn > horizon) return ReceiveStatus::too_far;
  if (packet.proposed_ms <= 0) return ReceiveStatus::malformed;
  for (std::size_t index = 0; index < packet.orders.size(); ++index) {
    if (packet.orders[index].issuer != packet.peer ||
        packet.orders[index].sequence != static_cast<std::uint32_t>(index) ||
        !is_wire_kind(packet.orders[index].kind)) {
      return ReceiveStatus::malformed;
    }
  }

  const std::size_t offset = packet.turn - next_turn_;
  if (pending_.size() <= offset) {
    pending_.resize(offset + 1, std::vector<std::optional<TurnPacket>>(config_.peers.size()));
  }
  std::optional<TurnPacket>& held = pending_[offset][*slot];
  if (held.has_value()) {
    // Compared as bytes, which is the one comparison that cannot miss a field
    // the codec carries.
    return encode(*held) == encode(packet) ? ReceiveStatus::duplicate
                                           : ReceiveStatus::conflicting;
  }
  held = packet;
  return ReceiveStatus::accepted;
}

ReceiveStatus TurnNegotiator::receive(std::span<const std::byte> data) {
  TurnPacket packet;
  if (decode(data, packet) != DecodeStatus::ok) return ReceiveStatus::malformed;
  return receive(packet);
}

bool TurnNegotiator::ready() const noexcept {
  if (next_turn_ < config_.input_delay) return true;
  for (std::size_t slot = 0; slot < config_.peers.size(); ++slot) {
    // A departed peer not yet decided: nothing past what this peer reported,
    // even if the packet has since arrived, because the decision may end its
    // part before it.
    if (!stints_[slot].back().until.has_value() && suspended_[slot].has_value() &&
        next_turn_ >= *suspended_[slot]) {
      return false;
    }
    if (!required_slot(slot, next_turn_)) continue;
    if (pending_.empty() || !pending_.front()[slot].has_value()) return false;
  }
  return true;
}

std::optional<AgreedTurn> TurnNegotiator::take() {
  if (!ready()) return std::nullopt;
  AgreedTurn turn;
  turn.index = next_turn_;
  turn.real_ms = config_.initial_ms;
  if (next_turn_ >= config_.input_delay) {
    std::int32_t largest = 0;
    for (std::size_t slot = 0; slot < config_.peers.size(); ++slot) {
      for (const Stint& stint : stints_[slot]) {
        // The first turn without it: the computer takes the seat, written
        // into the turn so that every peer and every replay does it here.
        if (stint.until.has_value() && *stint.until == next_turn_) {
          NetOrder takeover;
          takeover.issuer = config_.peers[slot];
          takeover.kind = NetOrderKind::departed;
          turn.orders.orders.push_back(std::move(takeover));
        }
        // The first turn of a late joiner's part: the computer stops, the
        // same way.
        if (stint.from != 0 && stint.from == next_turn_) {
          NetOrder joined;
          joined.issuer = config_.peers[slot];
          joined.kind = NetOrderKind::joined;
          turn.orders.orders.push_back(std::move(joined));
        }
      }
      if (!required_slot(slot, next_turn_)) continue;
      std::optional<TurnPacket>& held = pending_.front()[slot];
      largest = std::max(largest, held->proposed_ms);
      for (NetOrder& order : held->orders) turn.orders.orders.push_back(std::move(order));
    }
    turn.real_ms = std::clamp(largest, config_.min_ms, config_.max_ms);
  }
  turn.game_speed = config_.game_speed;
  turn.length = turn_length_from_real_ms(turn.real_ms, config_.game_speed);
  // The speed of the turns after this one. A fixed speed takes none, and
  // none reaches the history either: refused on every peer alike.
  if (!config_.variable_speed) {
    std::erase_if(turn.orders.orders,
                  [](const NetOrder& order) { return order.kind == NetOrderKind::set_speed; });
  }
  // In canonical order, the order `apply_turn` writes the clock in. The turn
  // is assembled peer by peer and so is already in it -- walking it as it
  // stands was fault-injected and nothing could tell -- but the rule is
  // apply_turn's, and it is stated here the same way.
  for (const NetOrder* order : canonical_order(turn.orders)) {
    if (order->kind == NetOrderKind::set_speed) config_.game_speed = clamp_game_speed(order->speed);
  }
  // The window is indexed from `next_turn_`, so it moves with it -- over the
  // turns agreed in advance too, whose slots are empty because `receive`
  // refuses packets for them.
  if (!pending_.empty()) pending_.erase(pending_.begin());
  ++next_turn_;
  history_.turns.push_back(turn.orders);
  schedule_.push_back(turn.length);
  return turn;
}

// --------------------------------------------------------------------------
// the loopback check
// --------------------------------------------------------------------------

namespace {

/// The same small xorshift `imconform` synthesises its streams with. Not the
/// world's RNG: drawing from it would change the simulation being measured.
struct Draw {
  std::uint32_t state;
  std::uint32_t next() noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
};

struct InFlight {
  std::size_t due = 0;
  std::size_t to = 0;
  std::size_t sent = 0;  ///< send order, so that equal `due` delivers repeatably
  std::vector<std::byte> bytes;
};

struct Peer {
  PlayerId player = 0;
  std::unique_ptr<conformance::Run> run;
  std::unique_ptr<TurnNegotiator> negotiator;
  std::unique_ptr<LinkNode> node;  ///< with `relay` only
  conformance::Trace trace;
  std::size_t submitted = 0;  ///< how many of `intent`'s turns have been sent
  /// Left the match (`NetplayOptions::departures`): sends and takes nothing.
  bool gone = false;
  std::size_t left_round = 0;
  bool noticed = false;
  /// The match ended for this peer without it leaving: the coordinator went.
  bool ended = false;
  /// The next chat line expected from each speaker, to check the order.
  std::map<PlayerId, std::uint32_t> chat_next;
  /// A late joiner: the first turn it runs, and until its world has loaded,
  /// the save and the round it loads on.
  std::uint32_t first_turn = 0;
  std::vector<std::byte> state;
  std::size_t state_due = 0;
  /// A set_speed order to add to the next packet: the coordinator's, after
  /// seating a joiner.
  std::int32_t speed_due = 0;
  /// A late joiner that leaves again once it has run this many turns.
  std::size_t leave_after = 0;

  /// The turn after the last one this peer ran.
  [[nodiscard]] std::size_t reached() const noexcept { return first_turn + trace.entries.size(); }
};

/// A late joiner's progress, as the harness tracks it.
struct Seating {
  NetplayOptions::Join join;
  std::optional<std::uint32_t> from_turn;
  bool sent = false;  ///< the header, and the save, are on their way
};

}  // namespace

conformance::Divergence check_netplay(const conformance::Scenario& scenario, std::uint32_t seed,
                                      std::size_t turns, const CommandStream& intent,
                                      const NetplayOptions& options, NetplayReport* report,
                                      OrderVerifier* verifier) {
  NetplayReport local_report;
  NetplayReport& out_report = report != nullptr ? *report : local_report;
  out_report = NetplayReport{};
  conformance::Divergence out;

  LockstepConfig config = options.lockstep;
  std::sort(config.peers.begin(), config.peers.end());
  config.peers.erase(std::unique(config.peers.begin(), config.peers.end()), config.peers.end());
  out_report.peers = config.peers.size();
  if (config.peers.empty()) {
    out.kind = conformance::Divergence::Kind::unbuildable;
    return out;
  }

  if ((!options.departures.empty() || !options.joins.empty()) && !options.relay) {
    // The agreement travels in the link layer; without it there is none.
    out.kind = conformance::Divergence::Kind::unbuildable;
    return out;
  }
  if (!options.joins.empty() && options.snapshots == nullptr) {
    out.kind = conformance::Divergence::Kind::unbuildable;
    return out;
  }
  std::vector<Seating> seatings;
  for (const NetplayOptions::Join& join : options.joins) seatings.push_back(Seating{join, {}, false});
  // Peers a late joiner replaced: they left, and are compared as departed.
  std::vector<Peer> retired;

  std::vector<Peer> peers(config.peers.size());
  for (std::size_t i = 0; i < peers.size(); ++i) {
    peers[i].player = config.peers[i];
    // Each from its own seat, as a real peer is: see `Scenario::start_as`.
    peers[i].run = scenario.start_as(seed, config.peers[i]);
    if (peers[i].run == nullptr) {
      out.kind = conformance::Divergence::Kind::unbuildable;
      return out;
    }
    peers[i].negotiator = std::make_unique<TurnNegotiator>(config, config.peers[i]);
    if (options.relay) {
      // A star: the hub links to everyone, everyone else to the hub.
      std::vector<PlayerId> links;
      if (i == 0) {
        links.assign(config.peers.begin() + 1, config.peers.end());
      } else {
        links.push_back(config.peers.front());
      }
      peers[i].node = std::make_unique<LinkNode>(options.seed, config, config.peers[i], links);
    }
    peers[i].trace.scenario = std::string(scenario.name());
    peers[i].trace.seed = seed;
    // What `record` captures, so that the comparison with the unnetworked run
    // below does not report a pipeline difference that is only a blank field.
    for (const System* system : peers[i].run->world().systems()) {
      peers[i].trace.systems.emplace_back(system->name());
    }
  }

  Draw network{options.seed | 1u};
  std::vector<InFlight> wire;
  std::size_t sent = 0;
  const auto index_of = [&](PlayerId player) {
    return static_cast<std::size_t>(
        std::lower_bound(config.peers.begin(), config.peers.end(), player) - config.peers.begin());
  };
  const auto send = [&](std::size_t from, const TurnPacket& packet, std::size_t now) {
    if (peers[from].node != nullptr) {
      peers[from].node->offer(packet);
      return;
    }
    const std::vector<std::byte> bytes = encode(packet);
    for (std::size_t to = 0; to < peers.size(); ++to) {
      if (to == from) continue;
      const std::size_t copies = options.duplicate_every_packet ? 2 : 1;
      std::size_t due = now;
      for (std::size_t copy = 0; copy < copies; ++copy) {
        due += network.next() % (options.max_delay + 1);
        wire.push_back(InFlight{due, to, sent++, bytes});
      }
    }
  };

  // Each peer's own proposals are its own draw, so that the largest is not
  // always the same peer's and the agreed length is genuinely a maximum.
  std::vector<Draw> proposals;
  for (std::size_t i = 0; i < peers.size(); ++i) {
    proposals.push_back(
        Draw{((options.seed * 2654435761u) ^ (0x9e3779b9u * static_cast<std::uint32_t>(i + 1))) | 1u});
  }
  const auto propose = [&](std::size_t i) {
    if (options.relay && options.measured_proposals) return peers[i].node->proposal();
    if (!options.vary_proposals) return config.initial_ms;
    const std::int32_t span = std::max(1, config.max_ms - config.min_ms + 1);
    return config.min_ms + static_cast<std::int32_t>(proposals[i].next() % span);
  };

  const auto submit = [&](std::size_t i, std::size_t now) {
    Peer& peer = peers[i];
    std::vector<NetOrder> mine;
    if (const NetTurn* given = intent.at(peer.submitted); given != nullptr) {
      for (const NetOrder& order : given->orders) {
        if (order.issuer == peer.player) mine.push_back(order);
      }
    }
    if (options.players != nullptr && peer.run != nullptr) {
      for (NetOrder& order : options.players->orders(peer.player, peer.submitted, *peer.run)) {
        if (order.issuer == peer.player) mine.push_back(std::move(order));
      }
    }
    ++peer.submitted;
    if (peer.speed_due != 0) {
      NetOrder speed;
      speed.issuer = peer.player;
      speed.kind = NetOrderKind::set_speed;
      speed.speed = peer.speed_due;
      mine.push_back(speed);
      peer.speed_due = 0;
    }
    if (std::optional<TurnPacket> packet = peer.negotiator->submit(std::move(mine), propose(i));
        packet.has_value()) {
      send(i, *packet, now);
    }
  };

  // Generous: every turn can wait out the longest delay on both copies twice,
  // and with loss, several resends as well.
  std::size_t round_limit = (turns + config.input_delay + 1) * (2 * options.max_delay + 4) + 16;
  if (options.relay) round_limit = round_limit * (8 + options.loss_per_mille / 50) + 64;
  std::size_t round = 0;
  std::size_t idle = 0;
  const auto finished = [&] {
    return std::all_of(peers.begin(), peers.end(), [&](const Peer& p) {
      return p.gone || p.ended || p.reached() >= turns;
    });
  };
  // A peer leaves once it has run the turns its departure names. A late
  // joiner on the same seat is another player, and stays.
  const auto leave_if_due = [&](std::size_t i, std::size_t now) {
    for (const NetplayOptions::Departure& departure : options.departures) {
      if (departure.peer == peers[i].player && !peers[i].gone && peers[i].first_turn == 0 &&
          peers[i].trace.entries.size() >= departure.after_turns) {
        peers[i].gone = true;
        peers[i].left_round = now;
      }
    }
    if (peers[i].first_turn != 0 && peers[i].leave_after != 0 && !peers[i].gone &&
        peers[i].trace.entries.size() >= peers[i].leave_after) {
      peers[i].gone = true;
      peers[i].left_round = now;
    }
  };
  bool irreconcilable = false;
  // What a departure changes on every peer still playing: its links notice
  // it, and every node settles what it knows.
  const auto settle = [&](std::size_t now) {
    for (Peer& departed : peers) {
      if (!departed.gone || departed.noticed) continue;
      std::size_t notice = 2;
      for (const NetplayOptions::Departure& departure : options.departures) {
        if (departure.peer == departed.player) notice = departure.noticed_after;
      }
      if (now < departed.left_round + notice) continue;
      departed.noticed = true;
      for (Peer& peer : peers) {
        if (peer.gone || peer.node == nullptr) continue;
        const std::vector<PlayerId>& links = peer.node->links();
        if (std::find(links.begin(), links.end(), departed.player) != links.end()) {
          peer.node->depart(departed.player);
        }
      }
    }
    if (options.departures.empty()) return;
    for (Peer& peer : peers) {
      if (peer.gone || peer.ended || peer.node == nullptr) continue;
      const DepartureOutcome outcome = settle_departures(*peer.negotiator, *peer.node);
      if (outcome.state == DepartureOutcome::State::coordinator_left) {
        peer.ended = true;
        out_report.coordinator_left = true;
      } else if (outcome.state != DepartureOutcome::State::playing) {
        // Dropped while still playing, or handed a decision it cannot apply:
        // neither happens to an honest peer.
        irreconcilable = true;
      }
    }
  };

  for (std::size_t i = 0; i < peers.size(); ++i) {
    leave_if_due(i, 0);
    if (!peers[i].gone) submit(i, 0);
  }

  while (!finished()) {
    if (round >= round_limit) {
      out_report.deadlocked = true;
      break;
    }
    // Deliver what is due, in (due, send order): the harness's network is
    // random but the harness is not.
    std::stable_sort(wire.begin(), wire.end(), [](const InFlight& a, const InFlight& b) {
      return a.due != b.due ? a.due < b.due : a.sent < b.sent;
    });
    std::size_t delivered = 0;
    while (delivered < wire.size() && wire[delivered].due <= round) {
      const InFlight& packet = wire[delivered];
      Peer& to = peers[packet.to];
      out_report.bytes += packet.bytes.size();
      if (to.gone || to.ended) {
        ++delivered;
        continue;
      }
      if (to.node != nullptr) {
        const Delivery delivery =
            deliver(*to.negotiator, *to.node, std::span<const std::byte>(packet.bytes),
                    static_cast<std::uint32_t>(round * options.round_ms));
        const LinkReceipt& receipt = delivery.receipt;
        out_report.duplicates += receipt.duplicates;
        out_report.refused += receipt.refused + receipt.conflicting + delivery.refused;
        out_report.late += delivery.late;
        out_report.packets += receipt.packets.size();
        if (delivery.irreconcilable) irreconcilable = true;
        if (receipt.status == LinkReceipt::Status::departed_link) {
          ++out_report.late;
        } else if (receipt.status != LinkReceipt::Status::ok) {
          ++out_report.refused;
        }
        for (const ChatLine& line : receipt.chat) {
          ++out_report.chat_heard;
          std::uint32_t& next = to.chat_next[line.from];
          if (line.seq != next) ++out_report.chat_misordered;
          next = line.seq + 1;
        }
        // The link layer delivers each packet once, so a negotiator's refusal
        // is the two layers disagreeing -- except a departed peer's packet
        // past its end, which the negotiator knew of first: late.
        ++delivered;
        continue;
      }
      const ReceiveStatus status = to.negotiator->receive(std::span<const std::byte>(packet.bytes));
      ++out_report.packets;
      if (status == ReceiveStatus::duplicate) {
        ++out_report.duplicates;
      } else if (status != ReceiveStatus::accepted && status != ReceiveStatus::stale) {
        // A copy arriving after its turn ran is the retransmit's normal fate;
        // anything else is the protocol refusing something it was sent.
        ++out_report.refused;
      }
      ++delivered;
    }
    wire.erase(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(delivered));
    settle(round);
    if (irreconcilable) break;

    bool moved = false;
    // The coordinator seats every joiner whose seat has opened, and sends the
    // state once it has applied the turn before the joiner's first.
    const std::size_t hub = index_of(config.resolved_coordinator());
    for (Seating& seating : seatings) {
      Peer& host = peers[hub];
      if (host.gone || host.ended || host.node == nullptr) continue;
      if (!seating.from_turn.has_value()) {
        if (host.trace.entries.size() < seating.join.after_turns) continue;
        if (open_seat(*host.negotiator, *host.node) != seating.join.peer) continue;
        seating.from_turn = admit_joiner(*host.negotiator, *host.node, seating.join.peer);
        if (!seating.from_turn.has_value()) continue;
        out_report.joined.emplace_back(seating.join.peer, *seating.from_turn);
        host.speed_due = seating.join.speed;
        moved = true;
      }
    }
    for (std::size_t i = 0; i < peers.size(); ++i) {
      Peer& peer = peers[i];
      // A late joiner's world arrives: from here on it runs.
      if (peer.run == nullptr && !peer.state.empty() && round >= peer.state_due) {
        peer.run = options.snapshots->resume_as(seed, peer.state, peer.player);
        peer.state.clear();
        if (peer.run == nullptr) {
          out.kind = conformance::Divergence::Kind::unbuildable;
          return out;
        }
        moved = true;
      }
      if (i == hub) {
        for (Seating& seating : seatings) {
          if (!seating.from_turn.has_value() || seating.sent || peer.gone || peer.ended ||
              peer.negotiator->next_turn() != *seating.from_turn) {
            continue;
          }
          // The host has applied every turn before the joiner's first and
          // runs none further until the state is on its way.
          seating.sent = true;
          const std::size_t seat = index_of(seating.join.peer);
          const std::vector<std::byte> save = options.snapshots->save(*peer.run);
          const JoinHeader header = join_header(*peer.negotiator, *peer.node, Start{},
                                                seating.join.peer, options.seed, save);
          Peer joiner;
          joiner.player = seating.join.peer;
          joiner.negotiator = std::make_unique<TurnNegotiator>(header.start.config,
                                                               joiner.player, header.resume());
          joiner.node = std::make_unique<LinkNode>(options.seed, header.start.config, joiner.player,
                                                   std::vector<PlayerId>{peer.player},
                                                   header.resume(), header.chat_next);
          joiner.first_turn = header.from_turn;
          joiner.submitted = header.from_turn;
          joiner.state = save;
          joiner.chat_next = header.chat_next;
          joiner.leave_after = seating.join.leave_after;
          joiner.state_due = round + seating.join.transfer_rounds;
          joiner.trace.scenario = peer.trace.scenario;
          joiner.trace.seed = seed;
          joiner.trace.systems = peer.trace.systems;
          peer.node->add_link(joiner.player, header.from_turn,
                              peer.negotiator->held_from(header.from_turn));
          // What was on its way to the seat's last occupant is not the
          // joiner's to receive.
          std::erase_if(wire, [seat](const InFlight& flight) { return flight.to == seat; });
          retired.push_back(std::move(peers[seat]));
          peers[seat] = std::move(joiner);
          submit(seat, round);
          moved = true;
        }
      }
    }
    for (std::size_t i = 0; i < peers.size(); ++i) {
      Peer& peer = peers[i];
      if (peer.gone || peer.ended || peer.reached() >= turns || peer.run == nullptr) continue;
      if (i == hub) {
        bool holding = false;
        for (const Seating& seating : seatings) {
          holding = holding || (seating.from_turn.has_value() && !seating.sent &&
                                peer.negotiator->next_turn() == *seating.from_turn);
        }
        if (holding) continue;
      }
      std::optional<AgreedTurn> agreed = peer.negotiator->take();
      if (!agreed.has_value()) {
        ++out_report.stalls;
        continue;
      }
      moved = true;
      std::unique_ptr<NetCommandSink> sink =
          options.sinks != nullptr ? options.sinks->sink(*peer.run) : nullptr;
      std::unique_ptr<OrderVerifier> own =
          options.verifiers != nullptr ? options.verifiers->verifier(*peer.run) : nullptr;
      const NetTurnReport applied = apply_turn(peer.run->world(), agreed->orders,
                                               own != nullptr ? own.get() : verifier, sink.get());
      out_report.issued += applied.issued;
      peer.run->advance(agreed->length);
      conformance::TraceEntry entry;
      entry.turn = peer.run->turns();
      entry.length = agreed->length;
      entry.time = peer.run->time();
      entry.hashes = peer.run->hashes();
      entry.hashes.netcmds =
          stream_hash(peer.negotiator->history(), peer.negotiator->history().size());
      entry.hashes.hash_of_hashes = conformance::roll_up(entry.hashes);
      peer.trace.entries.push_back(entry);
      if (peer.first_turn != 0) ++out_report.joiner_turns;
      leave_if_due(i, round);
      // Having started turn n, schedule turn n + 1 + delay.
      if (!peer.gone && peer.reached() < turns) submit(i, round);
    }
    if (options.relay) {
      // Every node talks to every link every round, whether or not it has
      // anything new: the datagram that carries nothing still carries the
      // acknowledgements and the clock.
      for (std::size_t i = 0; i < peers.size(); ++i) {
        if (peers[i].gone || peers[i].ended) continue;
        LinkNode& node = *peers[i].node;
        if (options.chat_every != 0 && round % options.chat_every == 0) {
          ChatLine line;
          line.to = static_cast<ChatLine::To>(round % 3);
          line.target = config.peers[(i + 1) % config.peers.size()];
          line.text = "round " + std::to_string(round) + " from " + std::to_string(node.self());
          (void)node.say(std::move(line));
          ++out_report.chat_said;
        }
        out_report.most_held = std::max(out_report.most_held, node.held());
        for (const PlayerId link : node.links()) {
          std::vector<std::byte> bytes =
              node.datagram_for(link, static_cast<std::uint32_t>(round * options.round_ms));
          ++out_report.datagrams;
          if (network.next() % 1000 < options.loss_per_mille) {
            ++out_report.lost;
            continue;
          }
          const std::size_t due = round + 1 + network.next() % (options.max_delay + 1);
          wire.push_back(InFlight{due, index_of(link), sent++, std::move(bytes)});
        }
      }
    }
    if (finished()) {
      ++round;
      break;
    }
    idle = moved || !wire.empty() ? 0 : idle + 1;
    if (idle > 1) {
      // Nothing moved and nothing is in flight: nothing ever will.
      out_report.deadlocked = true;
      break;
    }
    ++round;
  }
  out_report.rounds = round;
  // The peers that stayed are the match; the first of them speaks for it.
  std::vector<const Peer*> stayed;
  for (const Peer& peer : peers) {
    if (!peer.gone && !peer.ended) stayed.push_back(&peer);
  }
  for (const Peer* peer : stayed) {
    if (peer->node != nullptr) out_report.still_held += peer->node->held();
  }
  if (irreconcilable || out_report.coordinator_left || stayed.empty()) {
    out_report.deadlocked = out_report.deadlocked && !out_report.coordinator_left;
    out.kind = conformance::Divergence::Kind::unbuildable;
    return out;
  }
  // The first that played from the start speaks for the match.
  const auto original = std::find_if(stayed.begin(), stayed.end(),
                                     [](const Peer* p) { return p->first_turn == 0; });
  if (original == stayed.end()) {
    out.kind = conformance::Divergence::Kind::unbuildable;
    return out;
  }
  const Peer& first = **original;
  out_report.turns = first.trace.entries.size();
  for (const Peer* peer : stayed) {
    out_report.turns = std::min(out_report.turns, peer->reached());
  }
  out_report.schedule = first.negotiator->schedule();
  for (const conformance::TraceEntry& entry : first.trace.entries) {
    out_report.hashes.push_back(entry.hashes.hash_of_hashes);
  }
  out_report.orders = first.negotiator->history().order_count();
  // Each departure, as the first peer's negotiator ended it: a seat that was
  // joined again ended the stint the departed player played.
  const auto end_of = [&first](const Peer& peer) -> std::optional<std::uint32_t> {
    for (const SeatStints& seat : first.negotiator->membership()) {
      if (seat.peer != peer.player) continue;
      for (const Stint& stint : seat.stints) {
        if (stint.from == peer.first_turn) return stint.until;
      }
    }
    return std::nullopt;
  };
  for (const std::vector<Peer>* group : {&retired, &peers}) {
    for (const Peer& peer : *group) {
      if (!peer.gone) continue;
      if (const std::optional<std::uint32_t> end = end_of(peer)) {
        out_report.dropped.emplace_back(peer.player, *end);
      }
    }
  }

  if (out_report.deadlocked) {
    out.kind = conformance::Divergence::Kind::unbuildable;
    return out;
  }

  for (const Peer* peer : stayed) {
    if (peer == &first) continue;
    if (peer->first_turn != 0) {
      // A late joiner: the same match over every turn it ran, from its first
      // -- and it ran to the end, or it never really joined.
      if (peer->reached() < first.trace.entries.size() || peer->trace.entries.empty()) {
        out.kind = conformance::Divergence::Kind::unbuildable;
        return out;
      }
      conformance::Trace ours = first.trace;
      ours.entries.erase(ours.entries.begin(),
                         ours.entries.begin() + static_cast<std::ptrdiff_t>(peer->first_turn));
      conformance::Trace theirs = peer->trace;
      theirs.entries.resize(std::min(theirs.entries.size(), ours.entries.size()));
      out = conformance::compare_traces(ours, theirs);
      if (out.diverged()) {
        out.left = "peer " + std::to_string(first.player);
        out.right = "late joiner " + std::to_string(peer->player);
        return out;
      }
      continue;
    }
    out = conformance::compare_traces(first.trace, peer->trace);
    if (out.diverged()) {
      out.left = "peer " + std::to_string(first.player);
      out.right = "peer " + std::to_string(peer->player);
      return out;
    }
  }
  // Every joiner the options asked for was seated, and ran.
  for (const Seating& seating : seatings) {
    if (!seating.sent) {
      out.kind = conformance::Divergence::Kind::unbuildable;
      return out;
    }
  }

  // A departed peer played the same game as the rest for every turn it ran
  // below the end the match agreed for it -- and was never dropped at all is
  // a harness that did not finish the agreement.
  for (const std::vector<Peer>* group : {&retired, &peers}) {
    for (const Peer& peer : *group) {
      if (!peer.gone) continue;
      const std::optional<std::uint32_t> end = end_of(peer);
      if (!end.has_value()) {
        out.kind = conformance::Divergence::Kind::unbuildable;
        return out;
      }
      // Over the turns it ran below its end, from where it came in.
      const std::size_t shared =
          std::min<std::size_t>(peer.trace.entries.size(), *end - peer.first_turn);
      conformance::Trace theirs = peer.trace;
      conformance::Trace ours = first.trace;
      theirs.entries.resize(shared);
      ours.entries.erase(ours.entries.begin(),
                         ours.entries.begin() + static_cast<std::ptrdiff_t>(peer.first_turn));
      ours.entries.resize(std::min(shared, ours.entries.size()));
      out = conformance::compare_traces(ours, theirs);
      if (out.diverged()) {
        out.left = "peer " + std::to_string(first.player);
        out.right = "departed peer " + std::to_string(peer.player);
        return out;
      }
    }
  }

  // The transport is transparent: the same agreed stream and schedule, fed
  // straight to one run, is the same match.
  // With the peers' sink, so that a seat is taken over and handed back here
  // where it was there.
  class SinkDriver final : public conformance::Driver {
   public:
    SinkDriver(const CommandStream& stream, OrderVerifier* verifier, const RunSinks* sinks,
               const RunVerifiers* verifiers)
        : stream_(&stream), verifier_(verifier), sinks_(sinks), verifiers_(verifiers) {}
    [[nodiscard]] std::uint64_t drive(conformance::Run& run, std::size_t turn) override {
      if (const NetTurn* orders = stream_->at(turn); orders != nullptr) {
        std::unique_ptr<NetCommandSink> sink = sinks_ != nullptr ? sinks_->sink(run) : nullptr;
        std::unique_ptr<OrderVerifier> own =
            verifiers_ != nullptr ? verifiers_->verifier(run) : nullptr;
        (void)apply_turn(run.world(), *orders, own != nullptr ? own.get() : verifier_, sink.get());
      }
      return stream_hash(*stream_, turn + 1);
    }

   private:
    const CommandStream* stream_;
    OrderVerifier* verifier_;
    const RunSinks* sinks_;
    const RunVerifiers* verifiers_;
  };
  SinkDriver driver(first.negotiator->history(), verifier, options.sinks, options.verifiers);
  const std::vector<std::int32_t>& schedule = first.negotiator->schedule();
  conformance::Trace direct =
      conformance::record(scenario, seed, conformance::Schedule(schedule), &driver);
  out = conformance::compare_traces(direct, first.trace);
  out.left = "without a network";
  out.right = "peer " + std::to_string(first.player);
  return out;
}

}  // namespace imperivm::core::sim
