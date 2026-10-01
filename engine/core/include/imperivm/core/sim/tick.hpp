#pragma once

// The deterministic tick loop: game time and the lockstep turn.
//
// Model and evidence: docs/engine/tick.md
//
// **The tick count is state; wall time is not.** Nothing here reads a clock,
// and nothing here can: the core has none (docs/engine/architecture.md). The
// platform decides *when* to advance a turn and how long the next one should
// be; the core decides what advancing one does.
//
// ## There is no fixed timestep
//
// It is tempting to model this as a constant dt, and it would be wrong. The
// nine `Logs/*/desync.txt` dumps in a retail install record
// `gametimetickend - gametimetickstart` — the length of the turn about to run
// — and it takes four different values across them:
//
//     800  x4     400  x3     799  x1     200  x1
//
// and `[HASHHISTORY]` shows a single session renegotiating it mid-game: one
// log goes 400, 400, ..., 460, 800, 800, another 400, ..., 291, 213, 200, 200.
// It is a lockstep turn length tracking measured network latency. So the
// quantum is **declared per turn and is part of world state**: peers that
// disagree about the length of turn N are running different simulations, which
// is exactly why the dump records it.
//
// Determinism does not come from the step being constant. It comes from it
// being *exact, integral and agreed*: game time is the running sum of the
// declared turn lengths, and integer addition does not care how the interval
// was cut up.
//
// ## What the units are
//
//   * **A game-time unit is a millisecond at 100% speed.** `DATA\CONST.INI`
//     declares `[VXTIME] GameSpeed = 1000` and lists the speed presets on the
//     same scale (`SlowSpeed 700`, `NormalSpeed 1000`, `FastSpeed 1400`,
//     `FastestSpeed 2000`).
//   * **`gamespeed` scales the turn.** Eight dumps run at 1000; the ninth runs
//     at 999 and is the one whose turn is 799 units long rather than 800.
//     `floor(800 * 999 / 1000) = 799`. So the turn is negotiated in real
//     milliseconds and converted to game time by the speed — see
//     `turn_length_from_real_ms`. That single odd pair is the clearest
//     evidence in the data that `gamespeed` is a per-mille rate and not an
//     enumeration.
//   * **Game time has unit resolution.** The dumps record
//     `gametimetickstart = gametime + 1`, and pump boundaries take values like
//     3751 and 3894; no coarser quantum divides them.
//
// ## The turn is the pump turn
//
// The dumps' `Tick` field counts command-pump turns and nothing else:
// `Tick == (number of `CVXCmdPump::Tick --- Time:` lines) + 1` in all nine,
// exactly, from `Tick=0x2` to `Tick=0x2179`, and `gametime` equals the last
// recorded pump boundary in all nine. There is no sub-turn tick in the
// original and there is none here.

#include <cstdint>

namespace imperivm::core::sim {

/// Game time, in the engine's own units. Signed and 64-bit: a session reaches
/// 1.7 million units in the retail dumps, and a replay must be able to
/// subtract two of these without wrapping.
using GameTime = std::int64_t;

/// Game-time units per real second at 100% speed. `CONST.INI`'s
/// `[VXTIME] GameSpeed`, and the scale every duration in the game data uses.
inline constexpr std::int32_t kGameTimeUnitsPerSecond = 1000;

/// `NormalSpeed` in `CONST.INI`. The presets around it are 700, 1400 and 2000.
inline constexpr std::int32_t kDefaultGameSpeed = 1000;

/// The turn length every dump opens with: eight turns at 200, 600, 1000,
/// 1400, 1800, 2200, 2600, 3000 before the negotiation moves it.
inline constexpr std::int32_t kDefaultTurnLength = 400;

/// The extremes observed across the nine dumps and their hash histories. Not
/// limits — nothing enforces them — but a value far outside this range is
/// worth a second look.
inline constexpr std::int32_t kMinObservedTurnLength = 200;
inline constexpr std::int32_t kMaxObservedTurnLength = 800;

/// The game-time span of a real-time interval at `game_speed`.
///
/// Rounds down, which is what `floor(800 * 999 / 1000) = 799` in the one
/// off-speed dump shows. Never returns zero for a positive input: a
/// zero-length turn would advance nothing and spin.
[[nodiscard]] std::int32_t turn_length_from_real_ms(std::int32_t real_ms,
                                                    std::int32_t game_speed) noexcept;

/// The options screen's speed position, `Settings.ini`'s `[Options]
/// GameSpeed`, and the per-mille speed it stands for. **The one place the two
/// scales meet**: the options screen, the start of a match and every path
/// that turns one into the other go through these two.
///
/// Read from `gbr.exe`, both directions:
///
///   * **position -> speed** is what the options screen (0x006e7ff0) sends as
///     a `CVXCmdSetSpeed` when options are applied during a match whose speed
///     is variable: `700 + option * 2301 / 100` (0x006e8085-0x006e80a4),
///     truncating toward zero as the machine's signed divide does.
///   * **speed -> position** is what the start of a match writes back into
///     the options (0x006e71e0, called from 0x005268e7 with the speed the
///     match starts at): `(speed - 700) * 100 / 2301`, truncating the same way.
///
/// So the shipped `GameSpeed=13` is **not a choice anybody made**: it is
/// `NormalSpeed` 1000 written back, `(1000 - 700) * 100 / 2301 = 13`. Sent
/// forward again it is `700 + 13 * 2301 / 100 = 999`, and at 999 an 800 ms
/// turn is 799 units -- the one off-speed dump in `tick.md`, which is a
/// player pressing OK on the options screen without moving the slider. The
/// round trip loses a unit; that loss is the original's.
///
/// The speed is returned **unclamped**, as the screen sends it; whoever
/// writes it to a clock clamps (`clamp_game_speed` in `sim/netcmds.hpp`, which
/// is 0x004e67c0's 1..100000). Computed in 64 bits and saturated to 32, so an
/// absurd option in a hand-edited file cannot overflow.
inline constexpr std::int32_t kSpeedOptionBase = 700;   ///< `SlowSpeed`: position 0
inline constexpr std::int32_t kSpeedOptionSpan = 2301;  ///< per mille across 100 positions
[[nodiscard]] constexpr std::int32_t game_speed_from_option(std::int32_t option) noexcept {
  const std::int64_t speed =
      kSpeedOptionBase + static_cast<std::int64_t>(option) * kSpeedOptionSpan / 100;
  return speed > 0x7FFFFFFF ? 0x7FFFFFFF
         : speed < -0x7FFFFFFF - 1 ? -0x7FFFFFFF - 1
                                    : static_cast<std::int32_t>(speed);
}
[[nodiscard]] constexpr std::int32_t option_from_game_speed(std::int32_t speed) noexcept {
  return static_cast<std::int32_t>((static_cast<std::int64_t>(speed) - kSpeedOptionBase) * 100 /
                                   kSpeedOptionSpan);
}

/// How a world measures time. **All of it is world state**: two peers that
/// disagree on any of it are running different simulations, which is why the
/// dump records both fields.
struct TickConfig {
  /// The length of the next turn, in game-time units. Renegotiated as latency
  /// moves; see the file header.
  std::int32_t turn_length = kDefaultTurnLength;
  /// Rate scalar in the `CONST.INI` scale: 1000 is real time.
  std::int32_t game_speed = kDefaultGameSpeed;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return turn_length > 0 && game_speed > 0;
  }
};

/// One lockstep turn, in the shape the dump's `[GAMETIME]` block records it.
///
/// All three relations below hold exactly in all nine dumps:
/// `gametimetickstart = gametime + 1`, `gametimetickend = start + length`, and
/// `gametime` is the boundary the turn began at.
struct Turn {
  /// 1 for the first turn. The dumps' `Tick` field is this plus one, because
  /// the dump is written at the head of a turn that has not run yet.
  std::uint64_t index = 0;
  std::int32_t length = 0;  ///< `gametimetickend - gametimetickstart`
  GameTime start = 0;       ///< `gametimetickstart`: the boundary plus one
  GameTime end = 0;         ///< `gametimetickend`
  GameTime time = 0;        ///< game time once the turn has run
};

/// The simulation clock: a turn counter and the game time it has accumulated.
///
/// `time()` is the running sum of the declared turn lengths. It is not a
/// function of the turn index, because the turns are not all the same length —
/// so a saved game carries the time as well as the count, exactly as the dump
/// does.
class Clock {
 public:
  Clock() = default;
  explicit Clock(TickConfig config) noexcept
      : config_(config.valid() ? config : TickConfig{}) {}

  [[nodiscard]] const TickConfig& config() const noexcept { return config_; }
  [[nodiscard]] GameTime time() const noexcept { return time_; }
  [[nodiscard]] std::uint64_t turns() const noexcept { return turn_.index; }
  /// The turn that last ran, or an index-zero turn if none has.
  [[nodiscard]] const Turn& turn() const noexcept { return turn_; }
  /// The length the next turn will take unless one is declared for it.
  [[nodiscard]] std::int32_t turn_length() const noexcept { return config_.turn_length; }

  /// Renegotiate the turn length. Takes effect from the next turn.
  void set_turn_length(std::int32_t length) noexcept {
    if (length > 0) config_.turn_length = length;
  }
  /// Changing speed does not disturb accumulated game time. It changes how
  /// many game-time units the *next* negotiated real-time turn is worth.
  void set_game_speed(std::int32_t speed) noexcept {
    if (speed > 0) config_.game_speed = speed;
  }

  /// Run one turn at the current length.
  const Turn& advance() noexcept { return advance(config_.turn_length); }
  /// Run one turn of a declared length, which becomes the current length.
  /// A non-positive length is refused and the current one used instead, so a
  /// bad value cannot stall the simulation.
  const Turn& advance(std::int32_t length) noexcept;

  void reset() noexcept {
    time_ = 0;
    turn_ = Turn{};
  }

  /// Put the clock back where a saved game left it.
  ///
  /// **The one operation that moves game time backwards, and the save is the
  /// only reason it exists.** Nothing in a running simulation may rewind, which
  /// is why `time_` has no setter; a load is not a running simulation, it is
  /// the construction of one.
  ///
  /// `turn` has to come back too, not just `time`. **Turn length is not
  /// constant in this engine** — 200, 400, 799 and 800 all occur across the
  /// nine retail `desync.txt` dumps, and two of them renegotiate it mid-session
  /// — so the turn index is not a function of the time, and the length of the
  /// turn that last ran is not a function of either. All three are state.
  ///
  /// The config is deliberately left alone. `TickConfig::turn_length` is the
  /// length the *next* turn will take, which a renegotiation can have moved
  /// since the last turn ran, so it is restored by whoever built this `Clock`
  /// rather than inferred from `turn.length` here.
  ///
  /// Nothing is validated: a `Clock` cannot know which `(time, turn)` pairs a
  /// real session could have produced, and refusing a save is the loader's job.
  /// `World::deserialize` checks the three relations the dumps record —
  /// `start == time - length + 1`, `end == start + length`, `turn.time == time`
  /// — and builds the `Turn` from them before calling this.
  void restore(GameTime time, const Turn& turn) noexcept {
    time_ = time;
    turn_ = turn;
  }

 private:
  TickConfig config_{};
  GameTime time_ = 0;
  Turn turn_{};
};

}  // namespace imperivm::core::sim
