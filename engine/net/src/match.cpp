#include "imperivm/net/match.hpp"

#include <algorithm>

namespace imperivm::net {

using namespace core::sim;

namespace {

std::optional<PlayerId> player_at(const std::map<PlayerId, Endpoint>& links,
                                  const Endpoint& endpoint) {
  for (const auto& [player, at] : links) {
    if (at == endpoint) return player;
  }
  return std::nullopt;
}

}  // namespace

// --------------------------------------------------------------------------
// the host's lobby
// --------------------------------------------------------------------------

HostLobby::HostLobby(UdpSocket& socket, Start base, std::vector<PlayerId> seats,
                     std::uint64_t content, std::string host_name)
    : socket_(&socket),
      base_(std::move(base)),
      fixed_seats_(std::move(seats)),
      content_(content),
      host_name_(std::move(host_name)) {}

std::vector<PlayerId> HostLobby::open_seats() const {
  std::vector<PlayerId> out;
  if (base_.rows.empty()) {
    for (const PlayerId seat : fixed_seats_) {
      if (seated_.count(seat) == 0) out.push_back(seat);
    }
    return out;
  }
  for (const SeatRow& row : base_.rows) {
    if (row.type == SeatRow::Type::open && seated_.count(row.slot) == 0) out.push_back(row.slot);
  }
  return out;
}

std::size_t HostLobby::seats() const noexcept {
  return base_.rows.empty() ? fixed_seats_.size() : seated_.size() + open_seats().size();
}

void HostLobby::unseat(PlayerId slot) {
  seated_.erase(slot);
  names_.erase(slot);
  for (SeatRow& row : base_.rows) {
    if (row.slot != slot) continue;
    row.type = SeatRow::Type::open;
    row.name.clear();
    row.ready = false;
  }
  roster_due_ = true;
}

void HostLobby::say(std::string text) {
  ChatLine line;
  line.from = base_.you;
  line.seq = said_++;
  line.text = std::move(text);
  if (line.text.size() > kMaxChatText) line.text.resize(kMaxChatText);
  chat_.push_back(std::move(line));
  roster_due_ = true;
}

void HostLobby::send_rosters() {
  for (const auto& [slot, at] : seated_) {
    Start roster = base_;
    roster.you = slot;
    roster.chat = chat_;
    const auto heard = heard_.find(slot);
    roster.chat_heard = heard == heard_.end() ? 0 : heard->second;
    (void)socket_->send(at, encode(roster, /*final=*/false));
  }
}

bool HostLobby::step(std::uint32_t now) {
  // A seated joiner's row the host turned into something else is a seat taken
  // away: tell the joiner, and free it.
  if (!base_.rows.empty()) {
    for (auto it = seated_.begin(); it != seated_.end();) {
      const PlayerId slot = it->first;
      const auto row = std::find_if(base_.rows.begin(), base_.rows.end(),
                                    [slot](const SeatRow& r) { return r.slot == slot; });
      if (row == base_.rows.end() || row->type != SeatRow::Type::human) {
        Refuse refuse;
        refuse.reason = Refuse::Reason::closed;
        (void)socket_->send(it->second, encode(refuse));
        names_.erase(slot);
        it = seated_.erase(it);
        roster_due_ = true;
      } else {
        ++it;
      }
    }
  }

  while (std::optional<Received> got = socket_->receive()) {
    const LobbyKind kind = lobby_kind(got->bytes);
    const std::optional<PlayerId> who = player_at(seated_, got->from);
    if (kind == LobbyKind::query) {
      Query query;
      if (decode(got->bytes, query) != DecodeStatus::ok) continue;
      Advert advert;
      advert.host = host_name_;
      advert.map = base_.map;
      advert.port = socket_->port();
      advert.open = static_cast<std::uint8_t>(std::min<std::size_t>(open_seats().size(), 255));
      advert.seats = static_cast<std::uint8_t>(std::min<std::size_t>(seats() + 1, 255));
      (void)socket_->send(got->from, encode(advert));
      continue;
    }
    if (who.has_value()) {
      if (kind == LobbyKind::hello) {
        roster_due_ = true;  // it missed the roster: send it again now
      } else if (kind == LobbyKind::choice) {
        Choice choice;
        if (decode(got->bytes, choice) != DecodeStatus::ok) continue;
        for (SeatRow& row : base_.rows) {
          if (row.slot == *who && apply_choice(row, choice)) roster_due_ = true;
        }
        // Its lines, each once and in order: the next it has not said is
        // logged, anything before it is a resend, anything past it waits.
        std::uint32_t& heard = heard_[*who];
        for (ChatLine line : choice.chat) {
          if (line.seq != heard) continue;
          line.from = *who;
          chat_.push_back(std::move(line));
          ++heard;
          roster_due_ = true;
        }
      } else if (kind == LobbyKind::refuse) {
        unseat(*who);
      }
      continue;
    }
    if (kind != LobbyKind::hello) continue;
    Hello hello;
    const DecodeStatus status = decode(got->bytes, hello);
    Refuse refuse;
    const std::vector<PlayerId> open = open_seats();
    if (status == DecodeStatus::bad_version) {
      refuse.reason = Refuse::Reason::protocol;
    } else if (status != DecodeStatus::ok) {
      continue;  // noise
    } else if (hello.content != content_) {
      refuse.reason = Refuse::Reason::content;
    } else if (open.empty()) {
      refuse.reason = Refuse::Reason::full;
    } else {
      const PlayerId seat = open.front();
      seated_[seat] = got->from;
      names_[seat] = hello.name;
      for (SeatRow& row : base_.rows) {
        if (row.slot != seat) continue;
        row.type = SeatRow::Type::human;
        row.name = hello.name;
        row.ready = false;
      }
      roster_due_ = true;
      continue;
    }
    (void)socket_->send(got->from, encode(refuse));
  }

  if (!base_.rows.empty() && (roster_due_ || now - last_roster_ >= kRosterIntervalMs)) {
    send_rosters();
    last_roster_ = now;
    roster_due_ = false;
  }

  if (base_.rows.empty()) return open_seats().empty();
  if (seated_.empty()) return false;
  return std::all_of(seated_.begin(), seated_.end(), [this](const auto& entry) {
    for (const SeatRow& row : base_.rows) {
      if (row.slot == entry.first) return row.ready;
    }
    return false;
  });
}

Lobby HostLobby::finish(std::uint32_t seed) {
  Lobby lobby;
  // An open seat nobody took is a closed one: a match is never started with
  // a seat waiting.
  for (SeatRow& row : base_.rows) {
    if (row.type == SeatRow::Type::open) row.type = SeatRow::Type::closed;
  }
  base_.seed = seed;
  // The host decides where a departed joiner's turns end: it is the relay
  // every joiner links to, and without it nothing could go on anyway.
  base_.config.coordinator = base_.you;
  base_.config.peers = {base_.you};
  base_.links.clear();
  for (const auto& [player, at] : seated_) {
    base_.config.peers.push_back(player);
    base_.links.push_back(player);
  }
  lobby.start = base_;
  lobby.links = seated_;
  lobby.content = content_;
  // A star: the host links to every joiner, every joiner to the host.
  for (const auto& [player, at] : seated_) {
    Start theirs = base_;
    theirs.you = player;
    theirs.links = {base_.you};
    lobby.pending[player] = encode(theirs);
    (void)socket_->send(at, lobby.pending[player]);
  }
  lobby.ok = !seated_.empty();
  if (!lobby.ok) lobby.error = "nobody has joined";
  return lobby;
}

void HostLobby::close() {
  Refuse refuse;
  refuse.reason = Refuse::Reason::closed;
  for (const auto& [slot, at] : seated_) (void)socket_->send(at, encode(refuse));
  seated_.clear();
  names_.clear();
}

// --------------------------------------------------------------------------
// the joiner's lobby
// --------------------------------------------------------------------------

JoinLobby::JoinLobby(UdpSocket& socket, Endpoint host, Hello hello)
    : socket_(&socket), host_(host), hello_(encode(hello)) {}

void JoinLobby::say(std::string text) {
  ChatLine line;
  line.seq = said_++;
  line.text = std::move(text);
  if (line.text.size() > kMaxChatText) line.text.resize(kMaxChatText);
  unheard_.push_back(std::move(line));
  choice_due_ = true;
}

void JoinLobby::leave(Refuse::Reason reason) {
  Refuse refuse;
  refuse.reason = reason;
  (void)socket_->send(host_, encode(refuse));
  if (result_.error.empty()) result_.error = std::string("left: ") + describe(reason);
}

JoinLobby::State JoinLobby::step(std::uint32_t now) {
  if (result_.ok) return State::started;
  if (!result_.error.empty()) return State::refused;
  if (!sent_ || now - last_ >= kLobbyResendMs || choice_due_) {
    // Hello until seated -- a lost hello or a lost first roster both end the
    // same way, with the next one -- then the choice, which doubles as the
    // sign of life the host's roster is answered with.
    if (!seated_) {
      (void)socket_->send(host_, hello_);
    } else {
      core::sim::Choice sent = choice_;
      // What the host has not shown it holds, and no more than a choice
      // carries; the rest follows once these are heard.
      for (const ChatLine& line : unheard_) {
        if (sent.chat.size() == kLobbyChatLines) break;
        sent.chat.push_back(line);
      }
      (void)socket_->send(host_, encode(sent));
      choice_due_ = false;
    }
    last_ = now;
    sent_ = true;
  }
  while (std::optional<Received> got = socket_->receive()) {
    if (!(got->from == host_)) continue;
    const LobbyKind kind = lobby_kind(got->bytes);
    if (kind == LobbyKind::refuse) {
      Refuse refuse;
      result_.error = decode(got->bytes, refuse) == DecodeStatus::ok
                          ? std::string("refused: ") + describe(refuse.reason)
                          : std::string("refused");
      return State::refused;
    }
    if (kind == LobbyKind::join_header) {
      // The match was already running: a late join (`sim/netjoin.hpp`).
      JoinHeader header;
      if (decode(got->bytes, header) != DecodeStatus::ok) {
        result_.error = "the host's late-join header did not decode: a different build?";
        return State::refused;
      }
      result_.start = header.start;
      result_.links[header.start.links.empty() ? PlayerId{0} : header.start.links.front()] = host_;
      result_.join = std::move(header);
      result_.ok = true;
      return State::started;
    }
    if (kind == LobbyKind::roster) {
      Start roster;
      if (decode(got->bytes, roster) != DecodeStatus::ok) continue;
      roster_ = std::move(roster);
      std::erase_if(unheard_, [this](const ChatLine& line) { return line.seq < roster_.chat_heard; });
      if (!seated_) choice_due_ = true;
      seated_ = true;
      continue;
    }
    if (kind != LobbyKind::start) continue;
    if (decode(got->bytes, result_.start) != DecodeStatus::ok) {
      result_.error = "the host's start did not decode: a different build?";
      return State::refused;
    }
    for (const PlayerId link : result_.start.links) result_.links[link] = host_;
    result_.ok = true;
    return State::started;
  }
  return seated_ ? State::seated : State::waiting;
}

// --------------------------------------------------------------------------
// the LAN
// --------------------------------------------------------------------------

void LanBrowser::refresh(std::uint32_t now) {
  (void)now;
  socket_->enable_broadcast();
  const std::vector<std::byte> query = encode(Query{});
  (void)socket_->send(Endpoint{0xFFFFFFFFu, port_}, query);
  (void)socket_->send(Endpoint{0x7F000001u, port_}, query);
}

void LanBrowser::step(std::uint32_t now) {
  while (std::optional<Received> got = socket_->receive()) {
    if (lobby_kind(got->bytes) != LobbyKind::advert) continue;
    Advert advert;
    if (decode(got->bytes, advert) != DecodeStatus::ok) continue;
    // Where it answered from, at the port it says it hosts on.
    Endpoint at = got->from;
    at.port = advert.port;
    // One host on this machine answers twice -- the broadcast and the
    // loopback query both reach it -- from two addresses. The same advert on
    // the same port is the same game, and the LAN address is the one worth
    // showing: it is the one another machine could use too.
    const auto loopback = [](const Endpoint& e) { return (e.address >> 24) == 127; };
    const auto same = [&](const Found& f) {
      return f.at.port == at.port && f.advert.host == advert.host && f.advert.map == advert.map &&
             loopback(f.at) != loopback(at);
    };
    if (auto twin = std::find_if(games_.begin(), games_.end(), same); twin != games_.end()) {
      if (loopback(twin->at)) twin->at = at;
      twin->advert = advert;
      twin->seen = now;
      continue;
    }
    auto it = std::find_if(games_.begin(), games_.end(), [&](const Found& f) { return f.at == at; });
    if (it == games_.end()) {
      games_.push_back(Found{at, advert, now});
    } else {
      it->advert = advert;
      it->seen = now;
    }
  }
  std::erase_if(games_, [now](const Found& f) { return now - f.seen > 5000; });
}

// --------------------------------------------------------------------------
// the command line's forms
// --------------------------------------------------------------------------

Lobby host_lobby(UdpSocket& socket, const Start& base, std::vector<PlayerId> seats,
                 std::uint64_t content, std::uint32_t timeout_ms) {
  HostLobby lobby(socket, base, std::move(seats), content);
  const std::uint32_t begun = now_ms();
  while (!lobby.step(now_ms())) {
    if (now_ms() - begun > timeout_ms) {
      Lobby failed;
      failed.error = "timed out with " + std::to_string(lobby.seated().size()) + " of " +
                     std::to_string(lobby.seats()) + " joiners";
      return failed;
    }
    socket.wait(20);
  }
  return lobby.finish(base.seed);
}

Lobby join_lobby(UdpSocket& socket, const Endpoint& host, const Hello& hello,
                 std::uint32_t timeout_ms) {
  JoinLobby lobby(socket, host, hello);
  Choice ready;
  ready.ready = true;
  lobby.set_choice(ready);
  const std::uint32_t begun = now_ms();
  for (;;) {
    const std::uint32_t now = now_ms();
    const JoinLobby::State state = lobby.step(now);
    if (state == JoinLobby::State::started || state == JoinLobby::State::refused) {
      return lobby.result();
    }
    if (now - begun > timeout_ms) {
      Lobby failed;
      failed.error = "no answer from " + host.str();
      return failed;
    }
    socket.wait(20);
  }
}

// --------------------------------------------------------------------------
// the match
// --------------------------------------------------------------------------

namespace {

TurnNegotiator negotiator_for(const Lobby& lobby) {
  if (lobby.join.has_value()) {
    return TurnNegotiator(lobby.start.config, lobby.start.you, lobby.join->resume());
  }
  return TurnNegotiator(lobby.start.config, lobby.start.you);
}

LinkNode node_for(const Lobby& lobby) {
  if (lobby.join.has_value()) {
    return LinkNode(lobby.join->match, lobby.start.config, lobby.start.you, lobby.start.links,
                    lobby.join->resume(), lobby.join->chat_next);
  }
  return LinkNode(lobby.start.match, lobby.start.config, lobby.start.you, lobby.start.links);
}

}  // namespace

NetMatch::NetMatch(UdpSocket& socket, const Lobby& lobby)
    : socket_(&socket),
      negotiator_(negotiator_for(lobby)),
      node_(node_for(lobby)),
      links_(lobby.links),
      pending_(lobby.pending),
      start_(lobby.start),
      content_(lobby.content) {
  if (lobby.join.has_value()) {
    // A late joiner's start carries no match id of its own; the header does.
    start_.match = lobby.join->match;
    transfer_.emplace(*lobby.join);
    ack_due_ = true;  // the header arrived: say so, and stop its resend
  }
}

void NetMatch::send(const Endpoint& to, const std::vector<std::byte>& bytes) {
  ++stats_.sent;
  if (lose()) {
    ++stats_.dropped;
    return;
  }
  (void)socket_->send(to, bytes);
}

std::optional<AgreedTurn> NetMatch::take() {
  if (awaiting_state() || wants_state().has_value()) return std::nullopt;
  return negotiator_.take();
}

std::optional<std::uint32_t> NetMatch::wants_state() const noexcept {
  if (!seating_.has_value() || !state_.empty()) return std::nullopt;
  if (negotiator_.next_turn() != seating_->from_turn) return std::nullopt;
  return seating_->from_turn;
}

std::optional<std::span<const std::byte>> NetMatch::state() const noexcept {
  if (!transfer_.has_value() || !transfer_->verified()) return std::nullopt;
  return transfer_->bytes();
}

void NetMatch::hello_from(const Endpoint& from, std::span<const std::byte> bytes) {
  Hello hello;
  const DecodeStatus status = decode(bytes, hello);
  Refuse refuse;
  if (status == DecodeStatus::bad_version) {
    refuse.reason = Refuse::Reason::protocol;
  } else if (status != DecodeStatus::ok) {
    ++stats_.strangers;
    return;
  } else if (hello.content != content_) {
    refuse.reason = Refuse::Reason::content;
  } else if (seating_.has_value()) {
    // One at a time. The one being seated says hello until its header comes;
    // another is not answered, and says hello until the seat is free.
    return;
  } else {
    const std::optional<PlayerId> seat = late_join_ ? open_seat(negotiator_, node_) : std::nullopt;
    if (!seat.has_value() && late_join_ && !node_.departed().empty() &&
        node_.drops().size() < node_.departed().size()) {
      // A departure is being agreed: a seat is about to open. Not answered
      // until it has; the joiner says hello again.
      return;
    }
    if (!seat.has_value()) {
      refuse.reason = Refuse::Reason::full;
    } else if (const std::optional<std::uint32_t> first = admit_joiner(negotiator_, node_, *seat)) {
      seating_ = Seating{*seat, hello.name, *first, 0, 0, false};
      seating_at_ = from;
      dirty_ = true;  // the admission goes out with the next datagram
      return;
    } else {
      refuse.reason = Refuse::Reason::full;
    }
  }
  (void)socket_->send(from, encode(refuse));
}

void NetMatch::provide_state(std::vector<std::byte> save, std::uint32_t now) {
  if (!wants_state().has_value() || save.empty()) return;
  state_ = std::move(save);
  const JoinHeader header =
      join_header(negotiator_, node_, start_, seating_->seat, start_.match, state_);
  header_ = encode(header);
  seating_->size = header.size;
  // From now on it is a link like any other: it is sent every packet from
  // its first turn on, and times out if it says nothing.
  links_[seating_->seat] = seating_at_;
  last_heard_[seating_->seat] = now;
  node_.add_link(seating_->seat, seating_->from_turn, negotiator_.held_from(seating_->from_turn));
  last_state_ = now - kJoinResendMs;
  send_state(now);
  dirty_ = true;
}

void NetMatch::send_state(std::uint32_t now) {
  if (!seating_.has_value() || state_.empty()) return;
  if (now - last_state_ < kJoinResendMs) return;
  last_state_ = now;
  if (!seating_->header_heard) {
    send(seating_at_, header_);
    return;
  }
  // The window past what it holds; resent whole until acknowledged.
  for (std::size_t sent = 0; sent < kJoinWindow; ++sent) {
    const std::size_t offset = seating_->acked + sent * kJoinChunk;
    if (offset >= state_.size()) break;
    JoinChunk chunk;
    chunk.match = start_.match;
    chunk.offset = static_cast<std::uint32_t>(offset);
    const std::size_t end = std::min(state_.size(), offset + kJoinChunk);
    chunk.bytes.assign(state_.begin() + static_cast<std::ptrdiff_t>(offset),
                       state_.begin() + static_cast<std::ptrdiff_t>(end));
    send(seating_at_, encode(chunk));
  }
}

bool NetMatch::lose() noexcept {
  if (drop_per_mille_ == 0) return false;
  draw_ ^= draw_ << 13;
  draw_ ^= draw_ >> 17;
  draw_ ^= draw_ << 5;
  return draw_ % 1000 < drop_per_mille_;
}

void NetMatch::send_all(std::uint32_t now) {
  for (const auto& [player, at] : links_) {
    const std::vector<std::byte> bytes = node_.datagram_for(player, now);
    ++stats_.sent;
    if (lose()) {
      ++stats_.dropped;
      continue;
    }
    (void)socket_->send(at, bytes);
  }
  last_sent_ = now;
  dirty_ = false;
}

void NetMatch::leave() {
  Refuse refuse;
  refuse.reason = Refuse::Reason::left;
  const std::vector<std::byte> bytes = encode(refuse);
  // Twice: it is the last thing this peer says, and nothing will resend it.
  for (int copy = 0; copy < 2; ++copy) {
    for (const auto& [player, at] : links_) (void)socket_->send(at, bytes);
  }
}

NetDeparture& NetMatch::departure(PlayerId player, Refuse::Reason reason) {
  // A seat a late joiner took can be left again: that is another departure.
  const std::uint32_t since = node_.since(player);
  for (NetDeparture& known : departures_) {
    if (known.player == player && known.since == since) return known;
  }
  NetDeparture fresh;
  fresh.player = player;
  fresh.since = since;
  fresh.reason = reason;
  departures_.push_back(fresh);
  return departures_.back();
}

void NetMatch::depart(PlayerId player, Refuse::Reason reason) {
  if (seating_.has_value() && seating_->seat == player) {
    // A joiner that left before it had its state: its part is still agreed
    // over, the way any departure is, and the seat is free again after.
    seating_.reset();
    state_.clear();
  }
  (void)departure(player, reason);
  links_.erase(player);
  pending_.erase(player);
  last_heard_.erase(player);
  node_.depart(player);
  dirty_ = true;
}

void NetMatch::settle() {
  // Peers this one learned of from somebody else's report.
  for (const PlayerId player : node_.departed()) {
    if (player != negotiator_.local()) (void)departure(player, Refuse::Reason::left);
  }
  const DepartureOutcome outcome = settle_departures(negotiator_, node_);
  // Every decision the negotiator holds, whichever call applied it: a
  // datagram's are applied as it arrives, before its packets.
  for (const Drop& drop : node_.decisions()) {
    if (negotiator_.local() == drop.departed) continue;
    for (NetDeparture& known : departures_) {
      if (known.player == drop.departed && known.since == drop.since) known.from_turn = drop.from_turn;
    }
  }
  if (outcome.news) dirty_ = true;
  if (outcome.state != DepartureOutcome::State::playing && !ended_.has_value()) {
    ended_ = outcome.state;
  }
}

void NetMatch::pump(std::uint32_t now) {
  if (!clock_started_) {
    for (const auto& [player, at] : links_) last_heard_[player] = now;
    clock_started_ = true;
  }
  while (std::optional<Received> got = socket_->receive()) {
    const std::optional<PlayerId> from = player_at(links_, got->from);
    const LobbyKind kind = lobby_kind(got->bytes);
    if (seating_.has_value() && got->from == seating_at_ && !from.has_value()) {
      // The late joiner before its link exists: its hello resent, its
      // acknowledgement of the header, or its leaving.
      if (kind == LobbyKind::refuse) {
        Refuse refuse;
        if (decode(got->bytes, refuse) == DecodeStatus::ok) {
          const PlayerId seat = seating_->seat;
          depart(seat, refuse.reason);
        }
      }
      continue;
    }
    if (!from.has_value()) {
      if (kind == LobbyKind::hello) {
        hello_from(got->from, got->bytes);
      } else {
        ++stats_.strangers;
      }
      continue;
    }
    last_heard_[*from] = now;
    if (kind == LobbyKind::join_ack) {
      JoinAck ack;
      if (decode(got->bytes, ack) == DecodeStatus::ok && ack.match == start_.match &&
          seating_.has_value() && *from == seating_->seat) {
        seating_->header_heard = true;
        const bool more = ack.received > seating_->acked;
        seating_->acked = std::max(seating_->acked, std::min(ack.received, seating_->size));
        if (seating_->acked >= seating_->size) {
          // All of it: the seat is the joiner's, and the next may be seated.
          seated_.push_back(*seating_);
          seating_.reset();
          state_.clear();
          header_.clear();
        } else if (more) {
          last_state_ = now - kJoinResendMs;  // the window moved: fill it now
        }
      }
      continue;
    }
    if (kind == LobbyKind::join_header || kind == LobbyKind::join_chunk) {
      // The joiner's side of the transfer. The header again means the
      // acknowledgement was lost.
      if (transfer_.has_value()) {
        if (kind == LobbyKind::join_chunk) {
          JoinChunk chunk;
          if (decode(got->bytes, chunk) == DecodeStatus::ok) (void)transfer_->receive(chunk);
        }
        ack_due_ = true;
      }
      continue;
    }
    if (kind == LobbyKind::hello) {
      // A joiner still in the lobby: its `Start` was lost. Answer it again
      // now rather than at the next resend.
      if (const auto it = pending_.find(*from); it != pending_.end()) {
        (void)socket_->send(got->from, it->second);
      }
      continue;
    }
    if (kind == LobbyKind::refuse) {
      Refuse refuse;
      if (decode(got->bytes, refuse) == DecodeStatus::ok) depart(*from, refuse.reason);
      continue;
    }
    if (kind != LobbyKind::none) {
      // The lobby's traffic arriving late -- a resent start, a last choice,
      // a roster. We are past it; it is not a link datagram.
      continue;
    }
    ++stats_.received;
    Delivery delivery = deliver(negotiator_, node_, got->bytes, now);
    LinkReceipt& receipt = delivery.receipt;
    if (delivery.irreconcilable && !ended_.has_value()) {
      ended_ = DepartureOutcome::State::irreconcilable;
    }
    if (receipt.status == LinkReceipt::Status::departed_link) {
      ++stats_.late;
      continue;
    }
    if (receipt.status != LinkReceipt::Status::ok) {
      ++stats_.refused;
      continue;
    }
    // Its first link-layer datagram is the joiner's acknowledgement of Start.
    pending_.erase(*from);
    stats_.duplicates += receipt.duplicates;
    stats_.refused += receipt.refused + receipt.conflicting;
    stats_.late += delivery.late;
    stats_.refused += delivery.refused;
    if (!receipt.chat.empty()) dirty_ = true;
    for (ChatLine& line : receipt.chat) heard_chat_.push_back(std::move(line));
    if (!receipt.packets.empty() || receipt.departures_changed) dirty_ = true;  // promptly
  }

  // A link that has said nothing for too long has gone, whether or not it
  // said so.
  if (silence_limit_ms_ != 0) {
    std::vector<PlayerId> silent;
    for (const auto& [player, heard] : last_heard_) {
      if (now - heard > silence_limit_ms_) silent.push_back(player);
    }
    for (const PlayerId player : silent) depart(player, Refuse::Reason::silent);
  }
  settle();

  if (!pending_.empty() && now - last_start_ >= kLobbyResendMs) {
    for (const auto& [player, bytes] : pending_) (void)socket_->send(links_.at(player), bytes);
    last_start_ = now;
  }
  if (dirty_ || now - last_sent_ >= kSendIntervalMs) send_all(now);
  send_state(now);
  if (ack_due_ && transfer_.has_value() && !links_.empty()) {
    JoinAck ack;
    ack.match = start_.match;
    ack.received = transfer_->received();
    send(links_.begin()->second, encode(ack));
    ack_due_ = false;
  }
}

ChatLine NetMatch::say(ChatLine line) {
  dirty_ = true;
  return node_.say(std::move(line));
}

bool NetMatch::submit(std::vector<NetOrder> orders, std::uint32_t now) {
  const std::optional<TurnPacket> packet = negotiator_.submit(std::move(orders), node_.proposal());
  if (!packet.has_value()) return false;
  node_.offer(*packet);
  dirty_ = true;
  pump(now);
  return true;
}

}  // namespace imperivm::net
