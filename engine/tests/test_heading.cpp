// The heading family: sim/heading.hpp, plus the two entry points that share its
// corpus -- `Unit::user` / `Unit::SetUser` and `Obj::IsInState`.
//
// The expected angles below are not hand-reasoned. They come from a
// double-precision transcription of `0x0051b720` -- `asin`, then the quadrant
// repair, with the same three constants the executable stores -- and they are
// the answers a bird gets on the original, one-degree truncations included.
// The integer search in `sim/heading.cpp` agreed with that transcription on
// 226,561 vectors: every one within 40 units of the origin, 200,000 spread
// over the map, and 20,000 within two units of an axis.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/heading.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// One animation, 200 units long, so that `IsInState` has something to be false
/// during. Deliberately minimal -- the numbers in it are not what these tests
/// turn on.
constexpr std::string_view kEntityXml = R"(<?xml version="1.0"?>
<entity name="bird" type="vx/unit" variations="1">
  <images>
    <image idx="1" file="fly.rle" drawmode="player_color" remaping="none" rows="1" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="1" anim_frame="1"/>
  </states>
  <anims>
    <anim idx="1" name="fly" startstate="1" endstate="1" frames="3" duration="200"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
    </anim>
  </anims>
</entity>)";

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

struct HeadingBench {
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  World world{TickConfig{100, kDefaultGameSpeed}};
  script::HostRegistry registry;
  HostContext context;

  HeadingBench() {
    REQUIRE(entity.ok());
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId spawn() {
    const ObjectId id = world.spawn(NativeClass::unit, &entity.value());
    (void)world.set_position(id, Point{100, 100});
    return id;
  }

  script::HostOutcome call(script::CallKind kind, const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(kind, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    CHECK(registry.entry(index).fn != nullptr);
    if (registry.entry(index).fn == nullptr) {
      return script::HostOutcome::failed("not implemented");
    }
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }
};

}  // namespace

/// `GetAngleByDir` measures off the **east** axis, toward `+Y`, the same sense
/// `GetDirByAngle` builds in.
///
/// `0x0051b7a3` calls the C runtime's `asin` (0x007641e0; its domain error is
/// reported under the name at 0x00832e80, `asin`) on `|y| / |v|`, and
/// `0x0051b7a8` unfolds the first-quadrant angle with `a`, `6.283 - a`,
/// `3.1415 - a` and `3.1415 + a`. Due south, `(0, -1)`, is **269** rather than
/// 270: `asin(1) * 180/3.1415` is 90.0027, and `6.283` less that truncates
/// down.
///
/// These answers used to be the ones an `acos` reading gives -- `(0, 1)` at 0,
/// `(-1, 0)` at 89 -- which made the two functions a reflection of each other
/// and left every eagle unable to turn (playtest #18).
TEST(heading_angle_by_dir_measures_off_east) {
  CHECK(angle_of_dir(Point{1, 0}) == 0);
  CHECK(angle_of_dir(Point{0, 1}) == 90);
  CHECK(angle_of_dir(Point{-1, 0}) == 180);
  CHECK(angle_of_dir(Point{0, -1}) == 269);

  CHECK(angle_of_dir(Point{1, 1}) == 45);
  CHECK(angle_of_dir(Point{-1, 1}) == 134);
  CHECK(angle_of_dir(Point{-1, -1}) == 225);
  CHECK(angle_of_dir(Point{1, -1}) == 314);

  CHECK(angle_of_dir(Point{3, -7}) == 293);
  CHECK(angle_of_dir(Point{-3, 7}) == 113);
  CHECK(angle_of_dir(Point{5, 12}) == 67);
  CHECK(angle_of_dir(Point{-5, 12}) == 112);
  CHECK(angle_of_dir(Point{5, -12}) == 292);
  CHECK(angle_of_dir(Point{-5, -12}) == 247);

  // Scale does not move the answer, which a fixed-point implementation has to
  // earn rather than assume.
  CHECK(angle_of_dir(Point{100, 0}) == 0);
  CHECK(angle_of_dir(Point{0, 100}) == 90);
  CHECK(angle_of_dir(Point{-100, 0}) == 180);
  CHECK(angle_of_dir(Point{0, -100}) == 269);
  CHECK(angle_of_dir(Point{32767, 32767}) == 45);
  CHECK(angle_of_dir(Point{-32767, -32767}) == 225);
  // An eagle's starting facing, `(0, 1)` normalised.
  CHECK(angle_of_dir(Point{0, 1024}) == 90);

  // The zero vector is the `L < 0.0001` branch at `0x0051b76d`, which returns
  // before it ever divides.
  CHECK(angle_of_dir(Point{0, 0}) == 0);
}

/// One unit off an axis is where a table would give itself away.
///
/// The angle for `(1, 32767)` is 89.9998 degrees and for `(1, -32767)` the
/// fixed-up value is 269.9989 -- `6.283 - asin(...)` -- so the two truncate to
/// 90 and 269. `(23000, 1)` and `(23000, -1)` sit either side of zero and come
/// back as 0 and 359. A sine table at Q30 cannot separate those near the
/// vertical, where `sin` is flat; a tangent table can, because `tan` is large
/// exactly there, and this is the case that says which one the file uses.
TEST(heading_angle_by_dir_is_exact_one_unit_off_the_axis) {
  CHECK(angle_of_dir(Point{23000, 1}) == 0);
  CHECK(angle_of_dir(Point{23000, -1}) == 359);
  CHECK(angle_of_dir(Point{-23000, 1}) == 179);
  CHECK(angle_of_dir(Point{-23000, -1}) == 180);

  CHECK(angle_of_dir(Point{1, 32767}) == 90);
  CHECK(angle_of_dir(Point{-1, 32767}) == 89);
  CHECK(angle_of_dir(Point{1, -32767}) == 269);
  CHECK(angle_of_dir(Point{-1, -32767}) == 270);
}

/// `GetDirByAngle` builds from the **east** axis, with a pi of 3.1415, and its
/// length is 99 as often as it is 100.
///
/// `0x0051b802` multiplies by 0.017452777777777779, which is `3.1415/180` and
/// not `pi/180`, so a quarter turn is 1.57075 radians rather than 1.5707963.
/// `cos` of that is 4.6e-5 rather than zero and `sin` is 0.9999999989 rather
/// than one -- and 100 times the second truncates to **99**. The cardinal
/// directions are therefore not `(0, 100)` and `(-100, 0)`, and a table built
/// on the true pi would say they were.
TEST(heading_dir_by_angle_carries_the_retail_pi) {
  CHECK((dir_of_angle(0) == Point{100, 0}));
  CHECK((dir_of_angle(90) == Point{0, 99}));
  CHECK((dir_of_angle(180) == Point{-99, 0}));
  CHECK((dir_of_angle(270) == Point{0, -99}));

  // A full turn is not the identity either, for the same reason `Rot(360)` is
  // not: 360 times 3.1415/180 is 6.2830, which is short of two pi.
  CHECK((dir_of_angle(360) == Point{99, 0}));

  CHECK((dir_of_angle(30) == Point{86, 49}));
  CHECK((dir_of_angle(45) == Point{70, 70}));
  CHECK((dir_of_angle(60) == Point{50, 86}));
  CHECK((dir_of_angle(120) == Point{-49, 86}));
  CHECK((dir_of_angle(135) == Point{-70, 70}));
  CHECK((dir_of_angle(225) == Point{-70, -70}));
  CHECK((dir_of_angle(315) == Point{70, -70}));
  CHECK((dir_of_angle(359) == Point{99, -1}));

  // The span the corpus reaches. `EAGLE_LESSER_HOVER.VS` computes
  // `360 * (start - circleSegs / 8) / circleSegs`, which is -45 when the
  // closest hover point is the first, and `360 * (start + circleSegs * 2 / 3)`,
  // which is 585 when it is the last.
  CHECK((dir_of_angle(-45) == Point{70, -70}));
  CHECK((dir_of_angle(-30) == Point{86, -49}));
  CHECK((dir_of_angle(390) == Point{86, 49}));
  CHECK((dir_of_angle(585) == Point{-70, -70}));
  CHECK((dir_of_angle(720) == Point{99, 0}));

  // Past the table the angle is reduced. Nothing shipped reaches this; what the
  // test is for is that it terminates and stays inside the table, which a
  // subtract-360-until-it-fits loop would not for an argument like this one.
  CHECK((dir_of_angle(1000000) == Point{17, -98}));
  CHECK((dir_of_angle(-1000000) == Point{17, 98}));
  // A negative reduction has to be lifted into `[0, 360)` and not merely taken
  // modulo, and thirteen degrees of the 360 can tell the difference: the table
  // holds a whole extra turn either side, and one turn of `3.1415/180` is
  // 0.0106 degrees, which is enough to move a component by one. -1050 reduces
  // to -330 without the lift and to 30 with it, and those are (86, 50) and
  // (86, 49).
  CHECK((dir_of_angle(-1050) == Point{86, 49}));
}

/// The two are inverses up to a degree, which is what lets a bird turn round.
///
/// `CROW_MOVE.VS` and `EAGLE_MOVE.VS` read `angle = GetAngleByDir(dir);
/// dir = GetDirByAngle(angle +- 30)` and call it a thirty-degree turn. It is one:
/// a round trip through the pair gives back the angle or one less, never more,
/// because `GetDirByAngle`'s components truncate toward zero and its pi is
/// short. So twelve left turns take a bird all the way round.
///
/// Under the old `acos` reading a round trip was a reflection about the
/// diagonal, and 30 and 60 degrees were fixed points of the composed turn:
/// `GetAngleByDir((86, 49))` answered 60, and 60 less 30 is 30 again. An eagle
/// that reached one flew straight until it left the map.
TEST(heading_angle_and_dir_are_inverses_to_a_degree) {
  const Point east = dir_of_angle(0);
  CHECK((east == Point{100, 0}));
  CHECK(angle_of_dir(east) == 0);
  CHECK(angle_of_dir(dir_of_angle(30)) == 29);
  CHECK(angle_of_dir(dir_of_angle(90)) == 90);

  for (std::int32_t d = 0; d < 360; ++d) {
    const std::int32_t back = angle_of_dir(dir_of_angle(d));
    const std::int32_t lost = (d - back + 360) % 360;
    CHECK(lost == 0 || lost == 1);
  }

  // The composed turn, exactly as the scripts write it, visits every twelfth
  // of the circle from an eagle's own starting facing.
  bool seen[12] = {};
  Point dir{0, 1024};
  for (int step = 0; step < 24; ++step) {
    std::int32_t angle = angle_of_dir(dir) - 30;
    if (angle < 0) angle += 360;
    dir = dir_of_angle(angle);
    seen[angle_of_dir(dir) / 30] = true;
  }
  for (const bool sector : seen) CHECK(sector);
}

/// `GetVecByDir` leaves the zero vector alone, where `point::SetLen` does not.
///
/// `0x0051b6de` branches on the square root and writes it -- zero -- into both
/// components; `0x00697d81` writes the requested length into *y* and zero into
/// *x*. Two functions that both rescale a vector, disagreeing about the one
/// input that has no direction. `HEN_IDLE.VS` is the site that can reach it:
/// `GetVecByDir(.pos - center, 500)` on a hen standing exactly on `center`.
TEST(heading_vec_by_dir_leaves_the_zero_vector_alone) {
  CHECK((vec_by_dir(Point{0, 0}, 500) == Point{0, 0}));
  CHECK((vec_by_dir(Point{0, 0}, 0) == Point{0, 0}));

  // An ordinary rescale, truncating toward zero on both signs.
  CHECK((vec_by_dir(Point{100, 0}, 75) == Point{75, 0}));
  CHECK((vec_by_dir(Point{0, -100}, 75) == Point{0, -75}));
  CHECK((vec_by_dir(Point{3, 4}, 100) == Point{60, 80}));
  CHECK((vec_by_dir(Point{-3, -4}, 100) == Point{-60, -80}));
  // 7/9 and 11/9 of a unit, both truncated toward zero rather than rounded.
  CHECK((vec_by_dir(Point{-7, 11}, 9) == Point{-4, 7}));

  // Shrinking to nothing is an answer, not a refusal.
  CHECK((vec_by_dir(Point{1000, 1000}, 1) == Point{0, 0}));
}

/// `OnLeft` is a cross product in **32-bit** arithmetic, and it wraps.
///
/// `0x0051b15d` multiplies two map coordinates into a signed 32-bit register
/// and subtracts. Two coordinates near the top of a large map multiply to about
/// `INT32_MAX / 2` each, so the difference overflows and the sign flips -- and
/// the sign is the whole answer. Widening to 64 bits would give the *right*
/// answer and the wrong bird.
TEST(heading_on_left_is_a_32_bit_cross_product) {
  // North, and a point to its west. `b.x * a.y - b.y * a.x` is
  // `(-1)(1) - 0 = -1`, so not to the left.
  CHECK(!on_left(Point{0, 1}, Point{-1, 0}));
  CHECK(on_left(Point{0, 1}, Point{1, 0}));
  CHECK(!on_left(Point{1, 0}, Point{0, 1}));
  // Exactly collinear is not left: the test is `> 0`, not `>= 0`.
  CHECK(!on_left(Point{0, 1}, Point{0, 5}));
  CHECK(!on_left(Point{0, 1}, Point{0, -5}));

  // The wrap. Both products are 0x40000000-ish and of opposite sign, so the
  // true difference is just over `INT32_MAX` and the 32-bit answer is negative.
  // 46341 * 46341 = 0x80000E8A, which is negative as a signed int by itself.
  CHECK(on_left(Point{-46341, 46341}, Point{46341, 46341}));
  // The same geometry one unit smaller does not overflow, and answers the
  // other way -- which is what makes the line above a measurement of the wrap
  // and not of the geometry.
  CHECK(!on_left(Point{-46340, 46340}, Point{46340, 46340}));
}

/// The five entry points are registered as free functions and answer through
/// the host table.
TEST(heading_hosts_are_reachable_by_name) {
  HeadingBench b;
  CHECK(heading_host_entry_count() == 5);

  const script::HostOutcome angle =
      b.call(script::CallKind::free_function, "GetAngleByDir", 1, {pack_point(Point{0, -1})});
  CHECK(angle.status == script::HostStatus::ok);
  CHECK(angle.value.is_integer() && angle.value.as_integer() == 269);

  const script::HostOutcome dir =
      b.call(script::CallKind::free_function, "GetDirByAngle", 1, {script::Value::integer(90)});
  CHECK(dir.status == script::HostStatus::ok);
  CHECK((unpack_point(dir.value) == Point{0, 99}));

  const script::HostOutcome vec = b.call(script::CallKind::free_function, "GetVecByDir", 2,
                                         {pack_point(Point{0, 0}), script::Value::integer(500)});
  CHECK(vec.status == script::HostStatus::ok);
  CHECK((unpack_point(vec.value) == Point{0, 0}));

  // `GetVec(from, to, len)`: the difference, scaled the way `GetVecByDir`
  // scales, and two coincident points answer the zero vector rather than
  // dividing by it.
  const script::HostOutcome along =
      b.call(script::CallKind::free_function, "GetVec", 3,
             {pack_point(Point{100, 100}), pack_point(Point{400, 500}), script::Value::integer(50)});
  CHECK(along.status == script::HostStatus::ok);
  CHECK((unpack_point(along.value) == Point{30, 40}));
  const script::HostOutcome still =
      b.call(script::CallKind::free_function, "GetVec", 3,
             {pack_point(Point{7, 7}), pack_point(Point{7, 7}), script::Value::integer(50)});
  CHECK((unpack_point(still.value) == Point{0, 0}));
  // Truncation, not rounding: (3, 4) scaled to 7 is (4, 5), not (4, 6).
  const script::HostOutcome cut =
      b.call(script::CallKind::free_function, "GetVec", 3,
             {pack_point(Point{0, 0}), pack_point(Point{3, 4}), script::Value::integer(7)});
  CHECK((unpack_point(cut.value) == Point{4, 5}));

  const script::HostOutcome left = b.call(script::CallKind::free_function, "OnLeft", 2,
                                          {pack_point(Point{0, 1}), pack_point(Point{1, 0})});
  CHECK(left.status == script::HostStatus::ok);
  CHECK(left.value.is_integer() && left.value.as_integer() != 0);

  // A wrong-typed argument is a refusal rather than an answer, which is what
  // keeps a compiler bug from reading as a bird flying east.
  const script::HostOutcome bad =
      b.call(script::CallKind::free_function, "GetAngleByDir", 1, {script::Value::integer(3)});
  CHECK(bad.status == script::HostStatus::error);
}

/// `Unit::user` is a script scratch int: written, read back, and untouched by
/// anything else.
TEST(heading_user_is_a_script_scratch_int) {
  HeadingBench b;
  const ObjectId id = b.spawn();

  const script::HostOutcome zero = b.call(script::CallKind::member, "user", 0, {HeadingBench::obj(id)});
  CHECK(zero.status == script::HostStatus::ok);
  CHECK(zero.value.is_integer() && zero.value.as_integer() == 0);

  CHECK(b.call(script::CallKind::member, "SetUser", 1,
               {HeadingBench::obj(id), script::Value::integer(1)})
            .status == script::HostStatus::ok);
  const script::HostOutcome one = b.call(script::CallKind::member, "user", 0, {HeadingBench::obj(id)});
  CHECK(one.value.is_integer() && one.value.as_integer() == 1);

  // No clamp: `0x005d7c80` writes what it is given, and `CROW_IDLE.VS` and the
  // Britain ship sequences between them only ever use 0 and 1, so a
  // range-checked field would be a rule nothing in the binary supports.
  CHECK(b.call(script::CallKind::member, "SetUser", 1,
               {HeadingBench::obj(id), script::Value::integer(-4242)})
            .status == script::HostStatus::ok);
  const script::HostOutcome odd = b.call(script::CallKind::member, "user", 0, {HeadingBench::obj(id)});
  CHECK(odd.value.is_integer() && odd.value.as_integer() == -4242);

  // An unresolvable receiver answers zero and writes nothing, rather than
  // refusing: the original reports into a sink that is a bare `ret`.
  const script::HostOutcome gone =
      b.call(script::CallKind::member, "user", 0, {HeadingBench::obj(9999)});
  CHECK(gone.status == script::HostStatus::ok);
  CHECK(gone.value.is_integer() && gone.value.as_integer() == 0);
  CHECK(b.call(script::CallKind::member, "SetUser", 1,
               {HeadingBench::obj(9999), script::Value::integer(7)})
            .status == script::HostStatus::ok);
}

/// `user` is hashed and it survives a save.
///
/// Both halves matter and neither is implied by the other. A field two peers
/// can disagree about without the hash noticing is a desync that surfaces three
/// turns later as a crow flying the wrong way; a field that a save drops is a
/// crow that reloads having forgotten it had decided to land.
TEST(heading_user_is_hashed_and_saved) {
  HeadingBench b;
  const ObjectId id = b.spawn();
  const std::uint64_t before = b.world.state_hash();
  b.world.mutable_state(id)->user = 1;
  CHECK(b.world.state_hash() != before);

  std::vector<std::byte> blob;
  b.world.serialize(blob);
  World reloaded{TickConfig{100, kDefaultGameSpeed}};
  const Status status = reloaded.deserialize(blob);
  CHECK(status.ok());
  const WorldObject* slot = reloaded.find(id);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.user == 1);
}

/// `IsInState` is the negation of "an animation is running", and the wait loop
/// two shipped scripts open with is what it has to terminate.
///
/// `CROW_MOVE.VS` and `EAGLE_MOVE.VS` both begin `while (!.IsInState) Sleep(50)`
/// before their first `PlayAnim`. Under the reading here that is a bird waiting
/// out whatever the idle script last started, and it ends when the animation
/// does. Under the opposite reading it would never end.
TEST(heading_is_in_state_is_the_absence_of_an_animation) {
  HeadingBench b;
  const ObjectId id = b.spawn();

  const auto in_state = [&] {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "IsInState", 0, {HeadingBench::obj(id)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };

  // Freshly spawned and holding a pose.
  CHECK(in_state());

  CHECK(b.world.play_anim(id, 1));
  CHECK(!in_state());

  // The animation is 200 units long; run past it and the object settles.
  b.world.advance_turns(4);
  CHECK(in_state());

  // A receiver that does not resolve answers false rather than refusing.
  const script::HostOutcome gone =
      b.call(script::CallKind::member, "IsInState", 0, {HeadingBench::obj(9999)});
  CHECK(gone.status == script::HostStatus::ok);
  CHECK(gone.value.is_integer() && gone.value.as_integer() == 0);
}

// --------------------------------------------------------------------------
// vec_of_angle, the length-bearing form of GetDirByAngle
// --------------------------------------------------------------------------

TEST(vec_of_angle_at_length_100_is_dir_of_angle_on_the_whole_circle) {
  // The int8 table `dir_of_angle` reads is `trunc(100 * cos(d * K))` per axis;
  // the Q30 table under `vec_of_angle` is the same constant at a different
  // precision. They must agree on every one of the 360 angles `rand(0, 359)`
  // can draw, or one of the two tables is wrong.
  for (std::int32_t d = 0; d < 360; ++d) {
    const Point table = dir_of_angle(d);
    const Point scaled = vec_of_angle(100, d);
    CHECK(table.x == scaled.x);
    CHECK(table.y == scaled.y);
  }
}

TEST(vec_of_angle_truncates_toward_zero_at_other_lengths) {
  // Values computed from `trunc(L * cos(d * 3.1415/180))` in double, which is
  // what the x87 sequence at 0x004292f6 produces up to the labelled residue.
  // `cos(90 * K)` is not zero under the short constant, so 200 at 90 degrees
  // is `(0, 199)`, not `(0, 200)`: the sine falls just short of 1 and truncates.
  CHECK((vec_of_angle(200, 90) == Point{0, 199}));
  CHECK((vec_of_angle(150, 45) == Point{106, 106}));
  CHECK((vec_of_angle(300, 180) == Point{-299, 0}));
  CHECK((vec_of_angle(191, 270) == Point{0, -190}));
  CHECK((vec_of_angle(1000, 359) == Point{999, -17}));
  CHECK((vec_of_angle(0, 123) == Point{0, 0}));
  // Reduced modulo 360, unlike the original; every caller passes 0..359.
  CHECK(vec_of_angle(150, 405) == vec_of_angle(150, 45));
  CHECK(vec_of_angle(150, -315) == vec_of_angle(150, 45));
}
