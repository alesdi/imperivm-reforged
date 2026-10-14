#pragma once

/// The simulation's one random number generator.
///
/// Rule 2 of docs/engine/architecture.md: **one RNG, explicitly seeded,
/// serialised as world state.** The original treats it that way and says so in
/// its desync dumps, whose `[SEEDS]` block records two counters:
///
///   * `syncseed` -- a 32-bit value, unrelated between files, which is the
///     deterministic generator's state. That is what `Rng::state()` is.
///   * `cmdidseed` -- 0x168 .. 0x1a30b, growing with `cmdsprocessed`. That is
///     not a generator at all but a monotone allocator for command ids, and it
///     lives on `World` (`World::next_command_id`) rather than here.
///
/// A system that wants randomness draws from `World::rng()`. It never owns one,
/// because two generators are two states to serialise and one of them will be
/// forgotten.
///
/// ## What the algorithm is
///
/// **The original's, recovered.** `gbr.exe` keeps its generator inside the
/// game object at `+0x12a0` (constructed by 0x0063feb0, vtable 0x007d8ee8),
/// and the vtable has exactly one drawing method: **Range(low, high)** at
/// 0x0063ff00 (slot `+0x14`). Its other slots set the state (`+0xc`,
/// 0x0063fed0, which writes `+4`), read it, and seed it from the clock. So
/// every draw the original takes is a Range, and this class is that method:
///
///   * `low == high` answers `low` **without advancing** the state;
///   * `low > high` swaps the two, so the answer lies in `[high, low]`, and
///     draws;
///   * otherwise the state advances once by Numerical Recipes' LCG --
///     `state * 1664525 + 1013904223`, stored back at `+4` -- and the answer
///     is `low` plus the **high 32 bits** of `(high - low + 1)` times the new
///     state taken as unsigned. The span is computed in 32 bits and
///     sign-extended into the 64-bit product (0x0063ff27..0x0063ff37).
///
/// There is no output scramble: the state *is* the draw. This engine had a
/// documented placeholder here -- the same LCG with a bijective finaliser,
/// and a draw on every call, empty range or not -- until the host audit read
/// `rand` (0x004c63a0), which is `Range(0, n - 1)` on this generator and
/// nothing else. `World::rng()` is the one at `+0x12a0`; `gbr.exe` has a
/// second instance at `+0x12b4` with the same code, which this engine does
/// not model separately.
///
/// ## `rand(n)` is exclusive
///
/// `below(n)` returns a value in `[0, n)`, which is the contract the shipped
/// scripts demand. Three independent call sites in `data.pak` settle it:
///
///   * `rand(ol.count)` (5 sites) indexes an `ObjList`, so `n` itself must be
///     unreachable.
///   * `pt.Rot(rand(360))` rotates by a whole circle of degrees.
///   * `if (rand(100) < 25)` is a 25% chance, which is exact only if the draw
///     is `[0, 99]`.
///
/// and `if (rand(2))` as a coin flip agrees. `rand(99)` -- three sites, all
/// `chance > rand(99)` -- then reads as a 1-in-99 scale rather than 1-in-100,
/// which is odd but is what the indexing evidence forces.
/// The body settles it too: `rand` passes `n - 1` as Range's inclusive high
/// end.

#include <cstdint>

namespace imperivm::core::sim {

class Rng {
 public:
  /// Any non-zero constant would do; this one is arbitrary and is only the
  /// value an unseeded world starts from. Real sessions seed explicitly.
  static constexpr std::uint32_t kDefaultSeed = 0x2545F491u;

  constexpr Rng() noexcept = default;
  explicit constexpr Rng(std::uint32_t seed) noexcept : state_(seed) {}

  /// The whole serialised state: the dump's `syncseed`.
  [[nodiscard]] constexpr std::uint32_t state() const noexcept { return state_; }
  constexpr void seed(std::uint32_t value) noexcept { state_ = value; }

  /// A full 32-bit draw: the advanced state itself. Advances exactly once.
  /// The original has no such method; it is for engine code that wants bits.
  std::uint32_t next() noexcept;

  /// `between(0, bound - 1)` -- the shipped `rand(n)` exactly. Uniform in
  /// `[0, bound)` for `bound >= 2`; `bound == 1` answers 0 **and does not
  /// draw**; `bound <= 0` is the swapped range `[bound - 1, 0]` and draws, so
  /// `rand(0)` is -1 or 0. Whether a call advances the stream is synchronised
  /// state, which is why the no-draw case is the original's and not a guard.
  std::int32_t below(std::int32_t bound) noexcept;

  /// The original's Range(low, high) (0x0063ff00), inclusive at both ends:
  /// `low == high` answers `low` without drawing, and `low > high` answers in
  /// `[high, low]`. See the header note.
  std::int32_t between(std::int32_t low, std::int32_t high) noexcept;

  friend constexpr bool operator==(const Rng&, const Rng&) noexcept = default;

 private:
  std::uint32_t state_ = kDefaultSeed;
};

}  // namespace imperivm::core::sim
