#pragma once

/// Headings: the four entry points that turn a direction into an angle and
/// back, and the integer arithmetic that makes them reproducible.
///
/// `GetAngleByDir/1` (4 sites), `GetDirByAngle/1` (16), `GetVecByDir/2` (17)
/// and `OnLeft/2` (6) are free functions, all in `data.pak`, and all four are
/// reached from exactly five scripts: `CROW_MOVE.VS`, `EAGLE_MOVE.VS`,
/// `EAGLE_HOVER.VS`, `EAGLE_LESSER_HOVER.VS` and `HEN_IDLE.VS`. Between them
/// they are how every bird in the game decides which way to point, which is
/// why they are one header rather than four lines in `world_host.cpp`.
///
/// ## The core has no floating point and these functions are trigonometry
///
/// `sim/world_host.cpp` already carries that argument for `point::Rot`, and the
/// conclusion is the same here: the reproduction is integer, exact where the
/// corpus lives, and labelled where it is not. What is *new* is that this
/// family uses **two more approximations of pi**, and neither is `Rot`'s.
/// Three constants, three functions, one binary:
///
///   | constant           | value                 | reads as        | used by            |
///   |--------------------|-----------------------|-----------------|--------------------|
///   | `0x7ac490`         | 0.017453292519444445  | 3.1415926535/180| `point::Rot`       |
///   | `0x7adf18`         | 0.017452777777777779  | **3.1415**/180  | `GetDirByAngle`    |
///   | `0x7beb58`         | 0.017444444444444446  | **6.28**/360    | `PickFlyingPoint`  |
///
/// and a fourth, `0x7be8a8` = 57.297469361769856 = **180/3.1415**, which is the
/// radians-to-degrees factor `GetAngleByDir` finishes with. They are not
/// interchangeable: `360 * (3.1415/180)` is 6.2830, which is 0.0106 degrees
/// short of a full turn, so a table built on one constant and used for another
/// is wrong by about one unit in a hundred.
///
/// ## `GetAngleByDir` and `GetDirByAngle` both measure from **+X**
///
/// Read off `0x0051b720` and `0x0051b7f0`, and they are inverses up to the
/// truncation and the short pi:
///
///     GetAngleByDir(v)  ->  trunc(f(asin(|v.y| / |v|)) * 180/3.1415)
///     GetDirByAngle(d)  ->  (trunc(100*cos(d*K)), trunc(100*sin(d*K)))
///
/// The routine `0x0051b7a3` calls is the C runtime's **`asin`**, not `acos`:
/// `0x007641e0` stores its argument and falls into `0x007641fd`, which builds
/// `sqrt((1 + x)(1 - x))` and takes `fpatan` of `x` over it -- `atan2(x,
/// sqrt(1 - x^2))`, the arcsine -- loads a constant for `|x| == 1`, and reports
/// a domain error under the name at `0x00832e80`, which is the string `asin`.
/// The one other caller, `0x0051b5b8` (the helper `PickFlyingPoint`'s turn
/// penalty uses), is the same routine.
///
/// `asin(|y|/|v|)` is the angle off the **east** axis, folded into the first
/// quadrant, and `0x0051b7a8` unfolds it the textbook way:
///
///     x >= 0, y >= 0   ->  a
///     x >= 0, y <  0   ->  6.283 - a
///     x <  0, y >= 0   ->  3.1415 - a
///     x <  0, y <  0   ->  3.1415 + a
///
/// So `(1, 0)` answers 0, `(0, 1)` 90, `(-1, 0)` 180 and `(0, -1)` 269 -- one
/// short, because `asin(1) * 180/3.1415` is 90.0027 and `6.283 - that` then
/// truncates down. `CROW_MOVE.VS` and `EAGLE_MOVE.VS` compose the two --
/// `angle = GetAngleByDir(dir); newDir = GetDirByAngle(angle + 30)` -- and get
/// a turn of thirty degrees, give or take the one the truncation loses.
///
/// **This file used to read the routine as `acos`**, which made the angle one
/// off north, the two functions a reflection about the diagonal rather than
/// inverses, and two of the four quadrant repairs look swapped. Composed, that
/// reflection has fixed points -- a heading at 30 or 60 degrees maps to itself
/// -- so a bird could not turn round, and every eagle flew off the map in a
/// straight line once `Unit::speed` stopped answering zero (playtest #18). The
/// quadrant repairs above were never a bug; they only looked like one against
/// the wrong function.
///
/// ## How the arithmetic is done here
///
/// Three tables, and the interesting one is the first.
///
/// **`asin` becomes a tangent search.** The result is always floored to a whole
/// degree, and in every quadrant the truncation of the *fixed-up* value is the
/// floor or the ceiling of `t`, so only those two matter, never `t` itself.
/// `t >= d` is `asin(|y|/L) >= d/K2`, which is `|y|/|x| >= tan(d/K2)`, which is
/// `|y| * 2^30 >= |x| * kTanQ30[d]` -- exact integers, no square root and no
/// division. The tests check it against a double-precision transcription of
/// `0x0051b720` with the executable's constants.
///
/// **`GetDirByAngle` becomes its own answers.** The length is the constant 100
/// baked into `0x0051b80c`, not an argument, so every possible result is a pair
/// of integers in `[-100, 100]` and the table holds them directly. That is
/// smaller than a Q30 sine table and, unlike one, cannot round differently from
/// the original in the last place.
///
/// **`GetVecByDir` and `OnLeft` need no tables at all.** Both are integer in the
/// executable too.
///
/// ## What is still unknown
///
/// The `GetDirByAngle` table spans `[-360, 720]`, which covers every angle the
/// shipped corpus can produce (`EAGLE_LESSER_HOVER.VS` reaches -45 at the low
/// end and 585 at the high). Outside that span the angle is reduced by whole
/// turns, which the original does **not** do -- it hands the raw product to
/// `fcos`, whose own reduction uses the true pi rather than 3.1415 -- so a
/// reduced angle is off by 0.0106 degrees per turn. No shipped site reaches it;
/// the day one does, the fix is more table.

#include <cstdint>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// `GetAngleByDir(v)`: degrees in `[0, 359]` off `+X` toward `+Y`, the same
/// sense `dir_of_angle` builds in. Zero for the zero vector, which is the
/// original's `L < 0.0001` branch.
[[nodiscard]] std::int32_t angle_of_dir(Point v) noexcept;

/// `GetDirByAngle(degrees)`: the vector of nominal length 100 at that angle,
/// measured from `+X` toward `+Y`.
[[nodiscard]] Point dir_of_angle(std::int32_t degrees) noexcept;

/// `(trunc(length * cos(d * K)), trunc(length * sin(d * K)))` -- the vector of
/// `length` at `degrees`, with the same constant `GetDirByAngle` uses and the
/// same truncation toward zero. `dir_of_angle(d)` is this at length 100.
///
/// It is the shape 0x004291d0 draws with: a garrisoned unit sent out to train
/// walks to `centre + vec_of_angle(radius + rand(64, 128), rand(0, 359))`,
/// which is `fild`, `fmul` by the constant at 0x007adf18, `fcos`/`fsin`,
/// `fmul` by the length and `_ftol` per axis. The constant is `3.1415/180`,
/// not `Rot`'s `3.1415926535/180`, so this cannot share `rotate_like_gbr`'s
/// table.
///
/// **Degrees are taken modulo 360**, which the original does not do: it feeds
/// the raw angle to the x87, and because the constant is short of `pi/180`
/// its `cos(360 * K)` is not 1. Every caller here passes `rand(0, 359)`, where
/// the two agree, and `dir_of_angle` keeps the unreduced span for the callers
/// that need it. The values are a Q30 table generated from the retail
/// constant, so a product can differ from the original's double by one unit
/// when the true product lies within about `2^-30 * length` of an integer --
/// the same labelled residue `rotate_like_gbr` carries. At length 100 the
/// table and `dir_of_angle` agree on all 360 entries, which the tests check.
[[nodiscard]] Point vec_of_angle(std::int32_t length, std::int32_t degrees) noexcept;

/// `GetVecByDir(v, length)`: `v` rescaled to `length`, truncating toward zero.
///
/// **The zero vector stays zero here**, where `point::SetLen` turns it into
/// `(0, length)`. Both are read off the binary -- `0x0051b6de` returns the
/// square root, which is zero, in both components -- and the difference is
/// real: `HEN_IDLE.VS` calls `GetVecByDir(.pos - center, 500)` on a hen that may
/// be standing exactly on `center`, and gets `(0, 0)` rather than a hen shoved
/// 500 units north.
[[nodiscard]] Point vec_by_dir(Point v, std::int32_t length) noexcept;

/// `OnLeft(a, b)`: whether `b` lies to the left of `a`.
///
/// `0x0051b15d` is `b.x * a.y - b.y * a.x > 0` in **32-bit** arithmetic, and
/// the products of two map coordinates are close enough to `INT32_MAX` that
/// the subtraction can wrap. The wrap is reproduced rather than widened: two
/// peers that disagree about which side of a crow a point is on would send the
/// crow different ways.
[[nodiscard]] bool on_left(Point a, Point b) noexcept;

/// Defines the four entry points above. Returns how many.
std::size_t register_heading_host(script::HostRegistry& registry);

/// How many entry points `register_heading_host` defines.
[[nodiscard]] std::size_t heading_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
