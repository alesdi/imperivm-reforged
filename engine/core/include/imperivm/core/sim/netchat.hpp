#pragma once

/// Chat: what players say to each other, and why none of it is the game.
///
/// Two screens carry it. `INGAMECHAT.INI` is the match's: an edit line, three
/// radio buttons -- *Send to all* (0x20101), *Send to allies* (0x20102), *Send
/// to player* (0x20103) with the player chosen in `PlayerCombo` (0x2009) --
/// and *Send* (0x1001), *Send location* (0x1007), *Cancel* (0x1002). The
/// command bar's `Chat` button says *Chat (Enter)*. `MPCHAT.INI` is the
/// players' screen's: a scrolling log (`ChatLog` 0x9002) over an edit line
/// (`ChatEdit` 0x9003). What they send is this engine's, since the original's
/// wire was DirectPlay's and is not reproduced.
///
/// ## Chat is not an input
///
/// A line never reaches the turn negotiator, the command stream or the world:
/// it travels beside the turn packets (`sim/netlink.hpp` in a match, the
/// lobby's own messages before one) and is shown by the screen. So it is in no
/// hash -- `check_netplay` plays one match with chat and one without and
/// requires every per-turn hash to be the same -- and a peer that drops or
/// garbles a line has lost a line, not the game. Whom a line is for is
/// decided where it is shown, from the viewer's own world, which every peer
/// holds identically; the lines themselves go to every peer, so a relay
/// forwards what it may not show (this engine's reading: the original's
/// routing is not recorded).
///
/// ## Delivery
///
/// Each speaker numbers its lines from zero. The link layer carries a
/// speaker's lines the way it carries turn packets -- every datagram repeats
/// what a link has not acknowledged, oldest first, and the acknowledgement is
/// "every line from P below N" -- but a line is only taken **in order**: one
/// past a gap is dropped and comes again. So every peer shows every line
/// once, in the order it was said, whatever the network did.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/sim/lockstep.hpp"

namespace imperivm::core::sim {

class PlayerTable;

/// The longest line, in bytes. `OB_GSCHAT.INI`'s edit line holds 200
/// (`BufSize = 200`), the only chat edit that says. Longer is cut on the way
/// out and refused on the way in.
inline constexpr std::size_t kMaxChatText = 200;

struct ChatLine {
  /// `INGAMECHAT.INI`'s three radio buttons.
  enum class To : std::uint8_t { all = 0, allies = 1, player = 2 };
  PlayerId from = 0;
  /// The speaker's own count, from zero.
  std::uint32_t seq = 0;
  To to = To::all;
  /// `To::player` only.
  PlayerId target = kNoPlayer;
  /// *Send location*: the point the speaker was looking at, in map units.
  bool located = false;
  Point location{};
  std::string text;
};

//     "IMCH"  u8 version  u8 from  u32 seq  u8 to  u8 target  u8 located
//     i32 x  i32 y  u8 length  length x char
inline constexpr std::uint8_t kChatVersion = 1;

[[nodiscard]] std::vector<std::byte> encode(const ChatLine& line);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, ChatLine& out);

/// Whether `viewer` is shown `line`. The speaker always sees its own; *all*
/// is everyone; *player* is the one chosen; *allies* is every player the
/// speaker is allied with, as the viewer's world says.
[[nodiscard]] bool shown_to(const ChatLine& line, PlayerId viewer, const PlayerTable& players);

}  // namespace imperivm::core::sim
