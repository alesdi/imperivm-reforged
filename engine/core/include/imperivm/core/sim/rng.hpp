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
/// ## What the algorithm is, and is not
///
/// **The original's generator has not been recovered.** Nine `syncseed` values
/// with no adjacent samples constrain nothing: there is no way to tell an LCG
/// from a xorshift from `rand()` out of the CRT with that. So this is a
/// deliberate, documented placeholder -- a 32-bit LCG with a bijective output
/// scramble, which is exact, integral, portable and has full period -- chosen so
/// that swapping it for the real one later is a change to two functions in
/// `rng.cpp` and nothing else. `state()` / `seed()` are the whole serialised
/// surface; nothing outside depends on how a draw is computed.
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

  /// A full 32-bit draw. Advances the state exactly once.
  std::uint32_t next() noexcept;

  /// Uniform in `[0, bound)` -- the shipped `rand(n)`. Zero for `bound <= 0`,
  /// and `bound == 1` still consumes a draw, because whether a call advances
  /// the stream is itself synchronised state.
  std::int32_t below(std::int32_t bound) noexcept;

  /// Uniform in `[low, high]`, inclusive at both ends. Empty ranges yield
  /// `low` and still draw, for the same reason.
  std::int32_t between(std::int32_t low, std::int32_t high) noexcept;

  friend constexpr bool operator==(const Rng&, const Rng&) noexcept = default;

 private:
  std::uint32_t state_ = kDefaultSeed;
};

}  // namespace imperivm::core::sim
