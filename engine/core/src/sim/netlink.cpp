#include "imperivm/core/sim/netlink.hpp"

#include <algorithm>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/sim/save.hpp"

namespace imperivm::core::sim {

// --------------------------------------------------------------------------
// the datagram
// --------------------------------------------------------------------------

std::vector<std::byte> encode(const Datagram& datagram) {
  std::vector<std::byte> out;
  for (const char c : {'I', 'M', 'L', 'D'}) bytes::put_u8(out, static_cast<std::uint8_t>(c));
  bytes::put_u8(out, kDatagramVersion);
  bytes::put_u32(out, datagram.match);
  bytes::put_u8(out, datagram.from);
  bytes::put_u32(out, datagram.stamp);
  bytes::put_u32(out, datagram.echo);
  bytes::put_u32(out, datagram.hold);
  bytes::put_u8(out, static_cast<std::uint32_t>(datagram.acks.size()));
  for (const LinkAck& ack : datagram.acks) {
    bytes::put_u8(out, ack.peer);
    bytes::put_u32(out, ack.next_turn);
  }
  bytes::put_u16(out, static_cast<std::uint32_t>(datagram.packets.size()));
  for (const std::vector<std::byte>& packet : datagram.packets) {
    bytes::put_u32(out, static_cast<std::uint32_t>(packet.size()));
    out.insert(out.end(), packet.begin(), packet.end());
  }
  bytes::put_u8(out, static_cast<std::uint32_t>(datagram.reports.size()));
  for (const DepartureReport& report : datagram.reports) {
    bytes::put_u8(out, report.departed);
    bytes::put_u8(out, report.reporter);
    bytes::put_u32(out, report.held);
    bytes::put_u32(out, report.since);
  }
  bytes::put_u8(out, static_cast<std::uint32_t>(datagram.drops.size()));
  for (const Drop& drop : datagram.drops) {
    bytes::put_u8(out, drop.departed);
    bytes::put_u32(out, drop.from_turn);
    bytes::put_u32(out, drop.since);
  }
  bytes::put_u8(out, static_cast<std::uint32_t>(datagram.admits.size()));
  for (const Admit& admit : datagram.admits) {
    bytes::put_u8(out, admit.peer);
    bytes::put_u32(out, admit.from_turn);
  }
  bytes::put_u8(out, static_cast<std::uint32_t>(datagram.chat_acks.size()));
  for (const LinkAck& ack : datagram.chat_acks) {
    bytes::put_u8(out, ack.peer);
    bytes::put_u32(out, ack.next_turn);
  }
  bytes::put_u16(out, static_cast<std::uint32_t>(datagram.chat.size()));
  for (const std::vector<std::byte>& line : datagram.chat) {
    bytes::put_u32(out, static_cast<std::uint32_t>(line.size()));
    out.insert(out.end(), line.begin(), line.end());
  }
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Datagram& out) {
  out = Datagram{};
  if (data.size() < 4) return DecodeStatus::truncated;
  if (!has_magic(data, "IMLD")) return DecodeStatus::bad_magic;
  ByteReader reader(data, 4);
  std::uint8_t version = 0;
  if (!reader.u8(version)) return DecodeStatus::truncated;
  if (version != kDatagramVersion) return DecodeStatus::bad_version;
  std::uint8_t from = 0;
  std::uint8_t acks = 0;
  if (!reader.u32(out.match) || !reader.u8(from) || !reader.u32(out.stamp) ||
      !reader.u32(out.echo) || !reader.u32(out.hold) || !reader.u8(acks)) {
    return DecodeStatus::truncated;
  }
  out.from = from;
  out.acks.resize(acks);
  for (LinkAck& ack : out.acks) {
    std::uint8_t peer = 0;
    if (!reader.u8(peer) || !reader.u32(ack.next_turn)) return DecodeStatus::truncated;
    ack.peer = peer;
  }
  std::uint16_t count = 0;
  if (!reader.u16(count)) return DecodeStatus::truncated;
  // Four bytes of length per packet at the least: believed no further.
  if (count > reader.remaining() / 4) return DecodeStatus::truncated;
  out.packets.reserve(count);
  for (std::uint16_t i = 0; i < count; ++i) {
    std::uint32_t length = 0;
    std::span<const std::byte> body;
    if (!reader.u32(length) || !reader.bytes(length, body)) return DecodeStatus::truncated;
    out.packets.emplace_back(body.begin(), body.end());
  }
  std::uint8_t reports = 0;
  if (!reader.u8(reports)) return DecodeStatus::truncated;
  out.reports.resize(reports);
  for (DepartureReport& report : out.reports) {
    std::uint8_t departed = 0;
    std::uint8_t reporter = 0;
    if (!reader.u8(departed) || !reader.u8(reporter) || !reader.u32(report.held) ||
        !reader.u32(report.since)) {
      return DecodeStatus::truncated;
    }
    report.departed = departed;
    report.reporter = reporter;
  }
  std::uint8_t drops = 0;
  if (!reader.u8(drops)) return DecodeStatus::truncated;
  out.drops.resize(drops);
  for (Drop& drop : out.drops) {
    std::uint8_t departed = 0;
    if (!reader.u8(departed) || !reader.u32(drop.from_turn) || !reader.u32(drop.since)) {
      return DecodeStatus::truncated;
    }
    drop.departed = departed;
  }
  std::uint8_t admits = 0;
  if (!reader.u8(admits)) return DecodeStatus::truncated;
  out.admits.resize(admits);
  for (Admit& admit : out.admits) {
    std::uint8_t peer = 0;
    if (!reader.u8(peer) || !reader.u32(admit.from_turn)) return DecodeStatus::truncated;
    admit.peer = peer;
  }
  std::uint8_t chat_acks = 0;
  if (!reader.u8(chat_acks)) return DecodeStatus::truncated;
  out.chat_acks.resize(chat_acks);
  for (LinkAck& ack : out.chat_acks) {
    std::uint8_t peer = 0;
    if (!reader.u8(peer) || !reader.u32(ack.next_turn)) return DecodeStatus::truncated;
    ack.peer = peer;
  }
  std::uint16_t lines = 0;
  if (!reader.u16(lines)) return DecodeStatus::truncated;
  if (lines > reader.remaining() / 4) return DecodeStatus::truncated;
  out.chat.reserve(lines);
  for (std::uint16_t i = 0; i < lines; ++i) {
    std::uint32_t length = 0;
    std::span<const std::byte> body;
    if (!reader.u32(length) || !reader.bytes(length, body)) return DecodeStatus::truncated;
    out.chat.emplace_back(body.begin(), body.end());
  }
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

// --------------------------------------------------------------------------
// the node
// --------------------------------------------------------------------------

LinkNode::LinkNode(std::uint32_t match, const LockstepConfig& config, PlayerId self,
                   std::vector<PlayerId> links)
    : match_(match), config_(config), self_(self), links_(std::move(links)) {
  std::sort(config_.peers.begin(), config_.peers.end());
  config_.peers.erase(std::unique(config_.peers.begin(), config_.peers.end()),
                      config_.peers.end());
  std::sort(links_.begin(), links_.end());
  links_.erase(std::unique(links_.begin(), links_.end()), links_.end());
  links_.erase(std::remove(links_.begin(), links_.end(), self_), links_.end());
  for (const PlayerId peer : links_) {
    Link link;
    link.peer = peer;
    link_state_.push_back(link);
  }
  for (const PlayerId peer : config_.peers) held_[peer].next = config_.input_delay;
}

LinkNode::LinkNode(std::uint32_t match, const LockstepConfig& config, PlayerId self,
                   std::vector<PlayerId> links, const ResumePoint& resume,
                   const std::map<PlayerId, std::uint32_t>& chat_next)
    : LinkNode(match, config, self, std::move(links)) {
  // Nothing before the turn it starts at is any use to it.
  for (auto& [peer, held] : held_) held.next = std::max(held.next, resume.turn);
  for (const SeatStints& seat : resume.membership) {
    if (!is_peer(seat.peer)) continue;
    for (const Stint& stint : seat.stints) {
      if (stint.from != 0) {
        admits_.insert({seat.peer, stint.from});
        open_stint(seat.peer, stint.from);
      }
      if (stint.until.has_value()) drops_.try_emplace({seat.peer, stint.from}, *stint.until);
    }
  }
  for (const auto& [speaker, next] : chat_next) {
    if (is_peer(speaker)) chat_next_[speaker] = next;
  }
}

LinkNode::Link* LinkNode::link_of(PlayerId peer) noexcept {
  for (Link& link : link_state_) {
    if (link.peer == peer) return &link;
  }
  return nullptr;
}

const LinkNode::Link* LinkNode::link_of(PlayerId peer) const noexcept {
  return const_cast<LinkNode*>(this)->link_of(peer);
}

bool LinkNode::is_peer(PlayerId peer) const noexcept {
  return std::binary_search(config_.peers.begin(), config_.peers.end(), peer);
}

bool LinkNode::holds(PlayerId origin, std::uint32_t turn) const {
  const auto it = held_.find(origin);
  if (it == held_.end()) return false;
  return turn < it->second.next || it->second.above.count(turn) != 0;
}

void LinkNode::mark_held(PlayerId origin, std::uint32_t turn) {
  Held& held = held_[origin];
  if (turn < held.next) return;
  held.above.insert(turn);
  while (!held.above.empty() && *held.above.begin() == held.next) {
    held.above.erase(held.above.begin());
    ++held.next;
  }
}

std::uint32_t LinkNode::acked(const Link& link, PlayerId origin) const {
  const auto it = link.acked.find(origin);
  return it == link.acked.end() ? config_.input_delay : it->second;
}

void LinkNode::offer(const TurnPacket& packet) {
  if (packet.peer != self_ || holds(self_, packet.turn)) return;
  store_[{self_, packet.turn}] = Stored{encode(packet), kNoPlayer};
  mark_held(self_, packet.turn);
}

std::vector<std::byte> LinkNode::datagram_for(PlayerId peer, std::uint32_t now) {
  Datagram datagram;
  datagram.match = match_;
  datagram.from = self_;
  datagram.stamp = now;
  datagram.hold = kNoEcho;
  for (const auto& [origin, held] : held_) datagram.acks.push_back(LinkAck{origin, held.next});
  // Every membership fact, every time: few, idempotent, never retracted --
  // every stint's, because a node that learns a joiner's admission must also
  // learn the departure that freed the seat.
  for (const auto& [key, held] : reports_) {
    if (datagram.reports.size() == 0xFF) break;
    const auto& [departed, since, reporter] = key;
    datagram.reports.push_back(DepartureReport{departed, reporter, held, since});
  }
  for (const auto& [key, from_turn] : drops_) {
    if (datagram.drops.size() == 0xFF) break;
    datagram.drops.push_back(Drop{key.first, from_turn, key.second});
  }
  for (const auto& [peer, from_turn] : admits_) {
    if (datagram.admits.size() == 0xFF) break;
    datagram.admits.push_back(Admit{peer, from_turn});
  }

  const Link* link = link_of(peer);
  if (link == nullptr) return encode(datagram);
  if (link->heard) {
    datagram.echo = link->last_stamp;
    datagram.hold = now - link->stamp_received_at;
  }

  // Oldest turn first, across origins: the packet the far side is stalled on
  // is the one with the lowest turn, and the budget must not cut it off in
  // favour of one it will not need for a while.
  std::vector<const std::pair<const std::pair<PlayerId, std::uint32_t>, Stored>*> due;
  for (const auto& entry : store_) {
    const auto& [key, stored] = entry;
    // Not back where it came from. Not to its origin either, but that needs
    // no test: a peer's own packets are contiguous from the moment it offers
    // them, so its acknowledgement already covers every one.
    if (stored.from_link == peer) continue;
    if (key.second < acked(*link, key.first)) continue;
    due.push_back(&entry);
  }
  std::stable_sort(due.begin(), due.end(), [](const auto* a, const auto* b) {
    return a->first.second < b->first.second;
  });
  std::size_t used = 0;
  for (const auto* entry : due) {
    const std::size_t size = entry->second.bytes.size() + 4;
    if (!datagram.packets.empty() && used + size > kDatagramBudget) break;
    if (datagram.packets.size() == 0xFFFF) break;
    datagram.packets.push_back(entry->second.bytes);
    used += size;
  }
  // Chat in the room the turns leave: a line waits a datagram rather than
  // push a turn packet out. Each speaker's oldest first, which is the only
  // order the far side takes them in.
  for (const auto& [speaker, next] : chat_next_) {
    datagram.chat_acks.push_back(LinkAck{speaker, next});
  }
  for (const auto& [key, stored] : chat_store_) {
    // Not back where it came from -- which needs no test of its own, unlike
    // a turn packet: lines are taken in order only, so the datagram that
    // brought a line acknowledged it and every line before it. (Tried, fault
    // -injected: nothing could tell it was gone.)
    const auto acked = link->chat_acked.find(key.first);
    if (acked != link->chat_acked.end() && key.second < acked->second) continue;
    const std::size_t size = stored.bytes.size() + 4;
    if (used + size > kDatagramBudget || datagram.chat.size() == 0xFFFF) break;
    datagram.chat.push_back(stored.bytes);
    used += size;
  }
  return encode(datagram);
}

LinkReceipt LinkNode::receive(std::span<const std::byte> data, std::uint32_t now) {
  LinkReceipt receipt;
  Datagram datagram;
  if (decode(data, datagram) != DecodeStatus::ok) {
    receipt.status = LinkReceipt::Status::malformed;
    return receipt;
  }
  if (datagram.match != match_) {
    receipt.status = LinkReceipt::Status::wrong_match;
    return receipt;
  }
  Link* link = link_of(datagram.from);
  if (link == nullptr) {
    // A peer known to have gone was a link once: what it sent before going
    // is still arriving, and none of it is needed -- whatever of its packets
    // the match keeps, the reports that fixed its end were made without it.
    const std::vector<PlayerId> gone = departed();
    receipt.status = std::binary_search(gone.begin(), gone.end(), datagram.from)
                         ? LinkReceipt::Status::departed_link
                         : LinkReceipt::Status::unknown_link;
    return receipt;
  }

  // Newest stamp wins: a reordered datagram must not drag the echo backwards,
  // or the far side measures a round trip that includes our reordering.
  if (!link->heard || static_cast<std::int32_t>(datagram.stamp - link->last_stamp) > 0) {
    link->last_stamp = datagram.stamp;
    link->stamp_received_at = now;
    link->heard = true;
  }
  if (datagram.hold != kNoEcho) {
    const std::uint32_t elapsed = now - datagram.echo;
    if (elapsed >= datagram.hold && elapsed - datagram.hold < 0x7FFFFFFFu) {
      const std::uint32_t sample = elapsed - datagram.hold;
      link->srtt = link->srtt == 0 ? std::max<std::uint32_t>(sample, 1)
                                   : (7 * link->srtt + sample) / 8;
    }
  }
  for (const LinkAck& ack : datagram.acks) {
    if (!is_peer(ack.peer)) continue;
    std::uint32_t& known = link->acked.try_emplace(ack.peer, config_.input_delay).first->second;
    known = std::max(known, ack.next_turn);
  }
  for (const LinkAck& ack : datagram.chat_acks) {
    if (!is_peer(ack.peer)) continue;
    std::uint32_t& known = link->chat_acked.try_emplace(ack.peer, 0).first->second;
    known = std::max(known, ack.next_turn);
  }
  // Chat: a speaker's next line, and nothing past a gap -- it comes again,
  // after the one missing, so every line is shown once and in order.
  for (const std::vector<std::byte>& bytes : datagram.chat) {
    ChatLine line;
    if (decode(bytes, line) != DecodeStatus::ok || !is_peer(line.from) || line.from == self_) {
      ++receipt.refused;
      continue;
    }
    std::uint32_t& next = chat_next_.try_emplace(line.from, 0).first->second;
    if (line.seq != next) continue;  // held already, or past a gap
    chat_store_[{line.from, line.seq}] = Stored{bytes, datagram.from};
    ++next;
    receipt.chat.push_back(std::move(line));
  }

  // The membership facts first, so that a packet in the same datagram past a
  // newly learned end is counted late rather than learned, and one from a
  // newly admitted joiner is learned rather than counted late. Admissions
  // before departures reads naturally, but the order among the facts cannot
  // be told apart: every fact is keyed by its stint, `cut` asks `accepts`
  // afresh, and a joiner's packets are only ever stored after both. (Fault
  // -injected -- admissions moved after the drops -- and nothing noticed.)
  for (const Admit& admit : datagram.admits) {
    if (!is_peer(admit.peer) || admit.from_turn == 0) {
      ++receipt.refused;
      continue;
    }
    if (admits_.insert({admit.peer, admit.from_turn}).second) {
      receipt.departures_changed = true;
      open_stint(admit.peer, admit.from_turn);
    }
  }
  for (const DepartureReport& report : datagram.reports) {
    if (!is_peer(report.departed) || !is_peer(report.reporter) ||
        report.departed == report.reporter) {
      ++receipt.refused;
      continue;
    }
    const auto [it, inserted] = reports_.try_emplace(
        std::make_tuple(report.departed, report.since, report.reporter), report.held);
    if (inserted) {
      receipt.departures_changed = true;
      if (report.since == since(report.departed)) forget_link(report.departed);
    } else if (it->second != report.held) {
      ++receipt.conflicting;
      receipt.departures_changed = true;
    }
  }
  for (const Drop& drop : datagram.drops) {
    if (!is_peer(drop.departed)) {
      ++receipt.refused;
      continue;
    }
    const auto [it, inserted] =
        drops_.try_emplace(std::make_pair(drop.departed, drop.since), drop.from_turn);
    if (inserted) {
      receipt.departures_changed = true;
      if (drop.since == since(drop.departed)) forget_link(drop.departed);
      cut();
    } else if (it->second != drop.from_turn) {
      ++receipt.conflicting;
      receipt.departures_changed = true;
    }
  }

  for (const std::vector<std::byte>& bytes : datagram.packets) {
    TurnPacket packet;
    if (decode(bytes, packet) != DecodeStatus::ok || !is_peer(packet.peer) ||
        packet.peer == self_ || packet.turn < config_.input_delay) {
      ++receipt.refused;
      continue;
    }
    if (!accepts(packet.peer, packet.turn)) {
      ++receipt.late;
      continue;
    }
    const auto key = std::make_pair(packet.peer, packet.turn);
    if (holds(packet.peer, packet.turn)) {
      // Compared when we still have the bytes. Once pruned, every link has
      // acknowledged the first version and nobody needs the comparison.
      const auto it = store_.find(key);
      if (it != store_.end() && it->second.bytes != bytes) {
        ++receipt.conflicting;
      } else {
        ++receipt.duplicates;
      }
      continue;
    }
    store_[key] = Stored{bytes, datagram.from};
    mark_held(packet.peer, packet.turn);
    receipt.packets.push_back(std::move(packet));
  }
  prune();
  return receipt;
}

ChatLine LinkNode::say(ChatLine line) {
  line.from = self_;
  std::uint32_t& next = chat_next_.try_emplace(self_, 0).first->second;
  line.seq = next++;
  if (line.text.size() > kMaxChatText) line.text.resize(kMaxChatText);
  chat_store_[{self_, line.seq}] = Stored{encode(line), kNoPlayer};
  return line;
}

void LinkNode::prune() {
  for (auto it = chat_store_.begin(); it != chat_store_.end();) {
    const auto& [key, stored] = *it;
    bool everyone = true;
    for (const Link& link : link_state_) {
      if (link.peer == key.first || link.peer == stored.from_link) continue;
      const auto acked = link.chat_acked.find(key.first);
      if (acked == link.chat_acked.end() || acked->second <= key.second) {
        everyone = false;
        break;
      }
    }
    it = everyone ? chat_store_.erase(it) : std::next(it);
  }
  for (auto it = store_.begin(); it != store_.end();) {
    const auto& [key, stored] = *it;
    bool everyone = true;
    for (const Link& link : link_state_) {
      if (link.peer == key.first || link.peer == stored.from_link) continue;
      if (acked(link, key.first) <= key.second) {
        everyone = false;
        break;
      }
    }
    it = everyone ? store_.erase(it) : std::next(it);
  }
}

void LinkNode::forget_link(PlayerId peer) {
  links_.erase(std::remove(links_.begin(), links_.end(), peer), links_.end());
  link_state_.erase(std::remove_if(link_state_.begin(), link_state_.end(),
                                   [peer](const Link& link) { return link.peer == peer; }),
                    link_state_.end());
  // What waited only on that link's acknowledgement can go now.
  prune();
}

void LinkNode::cut() {
  for (auto it = store_.begin(); it != store_.end();) {
    it = accepts(it->first.first, it->first.second) ? std::next(it) : store_.erase(it);
  }
}

std::uint32_t LinkNode::since(PlayerId peer) const noexcept {
  std::uint32_t from = 0;
  for (auto it = admits_.lower_bound({peer, 0}); it != admits_.end() && it->first == peer; ++it) {
    from = it->second;
  }
  return from;
}

bool LinkNode::accepts(PlayerId peer, std::uint32_t turn) const noexcept {
  if (!is_peer(peer)) return false;
  // The stint the turn falls in: the latest that began at or before it.
  std::uint32_t from = 0;
  for (auto it = admits_.lower_bound({peer, 0}); it != admits_.end() && it->first == peer; ++it) {
    if (it->second <= turn) from = it->second;
  }
  if (turn < from + config_.input_delay) return false;
  const auto end = drops_.find({peer, from});
  return end == drops_.end() || turn < end->second;
}

void LinkNode::open_stint(PlayerId peer, std::uint32_t from_turn) {
  Held& held = held_[peer];
  held.next = std::max(held.next, from_turn + config_.input_delay);
  held.above.erase(held.above.begin(), held.above.lower_bound(held.next));
  while (!held.above.empty() && *held.above.begin() == held.next) {
    held.above.erase(held.above.begin());
    ++held.next;
  }
}

void LinkNode::admit(PlayerId peer, std::uint32_t from_turn) {
  if (!is_peer(peer) || peer == self_ || from_turn == 0) return;
  if (!admits_.insert({peer, from_turn}).second) return;
  open_stint(peer, from_turn);
}

std::vector<Admit> LinkNode::admits() const {
  std::vector<Admit> out;
  for (const auto& [peer, from_turn] : admits_) out.push_back(Admit{peer, from_turn});
  return out;
}

void LinkNode::add_link(PlayerId peer, std::uint32_t from_turn,
                        const std::vector<TurnPacket>& packets) {
  if (!is_peer(peer) || peer == self_ || link_of(peer) != nullptr) return;
  Link link;
  link.peer = peer;
  // It needs nothing before the turn it starts at, and no line said before it
  // came: it continues each speaker's numbering from where this node is.
  for (const PlayerId origin : config_.peers) link.acked[origin] = from_turn;
  link.chat_acked = chat_next_;
  link_state_.push_back(link);
  links_.insert(std::lower_bound(links_.begin(), links_.end(), peer), peer);
  // What this node held for those turns may already be forgotten here --
  // every other link had it -- so it is held again until the joiner has it.
  for (const TurnPacket& packet : packets) {
    if (packet.turn < from_turn || !accepts(packet.peer, packet.turn)) continue;
    store_.try_emplace({packet.peer, packet.turn}, Stored{encode(packet), kNoPlayer});
  }
}

void LinkNode::depart(PlayerId peer) {
  if (!is_peer(peer) || peer == self_) return;
  seen_.insert({peer, since(peer)});
  forget_link(peer);
}

void LinkNode::report(PlayerId departed, std::uint32_t held) {
  if (!is_peer(departed) || departed == self_) return;
  reports_.try_emplace(std::make_tuple(departed, since(departed), self_), held);
  forget_link(departed);
}

void LinkNode::decide(PlayerId departed, std::uint32_t from_turn) {
  if (!is_peer(departed) || departed == self_) return;
  if (!drops_.try_emplace(std::make_pair(departed, since(departed)), from_turn).second) return;
  forget_link(departed);
  cut();
}

std::vector<PlayerId> LinkNode::departed() const {
  std::set<PlayerId> all;
  for (const auto& [peer, stint] : seen_) {
    if (stint == since(peer)) all.insert(peer);
  }
  for (const auto& [key, held] : reports_) {
    if (std::get<1>(key) == since(std::get<0>(key))) all.insert(std::get<0>(key));
  }
  for (const auto& [key, from_turn] : drops_) {
    if (key.second == since(key.first)) all.insert(key.first);
  }
  return {all.begin(), all.end()};
}

std::map<std::pair<PlayerId, PlayerId>, std::uint32_t> LinkNode::reports() const {
  std::map<std::pair<PlayerId, PlayerId>, std::uint32_t> out;
  for (const auto& [key, held] : reports_) {
    const auto& [departed, stint, reporter] = key;
    if (stint == since(departed)) out[{departed, reporter}] = held;
  }
  return out;
}

std::map<PlayerId, std::uint32_t> LinkNode::drops() const {
  std::map<PlayerId, std::uint32_t> out;
  for (const auto& [key, from_turn] : drops_) {
    if (key.second == since(key.first)) out[key.first] = from_turn;
  }
  return out;
}

std::vector<Drop> LinkNode::decisions() const {
  std::vector<Drop> out;
  for (const auto& [key, from_turn] : drops_) out.push_back(Drop{key.first, from_turn, key.second});
  return out;
}

std::uint32_t LinkNode::srtt(PlayerId peer) const noexcept {
  const Link* link = link_of(peer);
  return link == nullptr ? 0 : link->srtt;
}

std::int32_t LinkNode::proposal() const noexcept {
  std::uint32_t slowest = 0;
  for (const Link& link : link_state_) slowest = std::max(slowest, link.srtt);
  if (slowest == 0) return config_.initial_ms;
  const std::uint64_t spread = 2ull * (config_.input_delay + 1);
  const std::uint64_t ms = (3ull * slowest + spread - 1) / spread;
  return static_cast<std::int32_t>(std::min<std::uint64_t>(ms, 0x7FFFFFFF));
}

}  // namespace imperivm::core::sim
