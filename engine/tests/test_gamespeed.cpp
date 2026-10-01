// The game speed a player chooses, and the local command path it travels by.
//
// What carries the reading:
//
//   * `Settings.ini`'s `GameSpeed` is a position, not a speed and not a turn
//     interval: the options screen sends `700 + option * 2301 / 100` per
//     mille (0x006e7ff0), and the start of a match writes the position back
//     from its speed as `(speed - 700) * 100 / 2301` (0x006e71e0). The
//     shipped 13 is 1000 written back, and 999 sent forward again -- and an
//     800 ms turn at 999 is the 799 of the one off-speed dump.
//   * A single-player match posts its speed like a networked one: the order
//     waits for the next turn, that turn's length is converted first, and
//     the speed converts every turn after it.

#include <cstdint>
#include <vector>

#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/tick.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace {

NetOrder speed_order(std::int32_t per_mille, PlayerId issuer = 0) {
  NetOrder order;
  order.issuer = issuer;
  order.kind = NetOrderKind::set_speed;
  order.speed = per_mille;
  return order;
}

/// What the app's unnetworked clock does each turn: the head, then advance.
std::int32_t local_turn(World& world, LocalOrders& orders, std::int32_t real_ms) {
  const LocalTurn turn = begin_local_turn(world, orders, real_ms);
  world.advance(turn.length);
  return turn.length;
}

}  // namespace

TEST(the_shipped_speed_position_is_normal_speed_written_back) {
  // 0x006e71e0 on `NormalSpeed`: (1000 - 700) * 100 / 2301 = 13.04, so 13.
  CHECK(option_from_game_speed(1000) == 13);
  // And forward again, as OK on the options screen sends it: 999, not 1000.
  CHECK(game_speed_from_option(13) == 999);
  // Which, on the dumps' 800 ms turn, is the one off-speed dump's length.
  CHECK(turn_length_from_real_ms(800, game_speed_from_option(13)) == 799);
  CHECK(turn_length_from_real_ms(800, 1000) == 800);
}

TEST(the_speed_position_spans_slow_to_the_fastest_preset) {
  CHECK(game_speed_from_option(0) == 700);    // `SlowSpeed`
  CHECK(game_speed_from_option(100) == 3001); // `Speed5` 3000, within the unit
  CHECK(game_speed_from_option(56) == 1988);  // 56 * 2301 = 128856, / 100 = 1288
  CHECK(option_from_game_speed(700) == 0);
  CHECK(option_from_game_speed(2000) == 56);  // 130000 / 2301 = 56.49
  CHECK(option_from_game_speed(3000) == 99);  // 230000 / 2301 = 99.95
  // The presets survive the round trip only to within the division's loss.
  CHECK(game_speed_from_option(option_from_game_speed(1400)) == 1390);
}

TEST(the_speed_conversions_truncate_toward_zero_as_the_machine_divides) {
  // Both bodies divide signed and correct the quotient toward zero; a floor
  // would differ on every negative, and a hand-edited file can hold one.
  CHECK(game_speed_from_option(-1) == 700 - 23);        // -2301 / 100 = -23.01 -> -23
  CHECK(option_from_game_speed(699) == 0);              // -100 / 2301 -> 0, not -1
  CHECK(option_from_game_speed(1) == -30);              // -69900 / 2301 = -30.37 -> -30
  // Wide enough that an absurd position cannot overflow on the way.
  CHECK(game_speed_from_option(2000000000) == 0x7FFFFFFF);
  CHECK(game_speed_from_option(-2000000000) == -0x7FFFFFFF - 1);
  CHECK(option_from_game_speed(0x7FFFFFFF) == 93328246);
}

TEST(local_orders_wait_for_the_next_turn_in_the_order_they_were_posted) {
  LocalOrders orders;
  CHECK(orders.empty());
  CHECK(!orders.speed_fixed());
  orders.post(speed_order(1400));
  orders.post(speed_order(700));
  CHECK(!orders.empty());
  const NetTurn turn = orders.take();
  REQUIRE(turn.orders.size() == 2);
  CHECK(turn.orders[0].sequence == 0);
  CHECK(turn.orders[1].sequence == 1);
  // In canonical order, the second posted is applied last and wins.
  World world;
  (void)apply_turn(world, turn);
  CHECK(world.clock().config().game_speed == 700);
  // Taken once: the next turn has nothing.
  CHECK(orders.empty());
  CHECK(orders.take().orders.empty());
  // Numbering starts over with each turn's orders.
  orders.post(speed_order(1000));
  CHECK(orders.take().orders[0].sequence == 0);
  orders.set_speed_fixed(true);
  CHECK(orders.speed_fixed());
}

TEST(a_posted_speed_converts_the_turns_after_the_one_it_ran_with) {
  World world;
  LocalOrders orders;
  CHECK(local_turn(world, orders, 800) == 800);
  // OK on the options screen at the shipped position, between turns.
  orders.post(speed_order(game_speed_from_option(13)));
  // The turn it runs with was converted before it applied...
  CHECK(local_turn(world, orders, 800) == 800);
  CHECK(world.clock().config().game_speed == 999);
  // ...and every one after is at the new speed: the dump's 799.
  CHECK(local_turn(world, orders, 800) == 799);
  CHECK(local_turn(world, orders, 800) == 799);
  CHECK(world.time() == 800 + 800 + 799 + 799);
}

TEST(the_head_of_a_local_turn_reports_what_it_applied) {
  World world;
  LocalOrders orders;
  orders.post(speed_order(2000, 3));
  const LocalTurn turn = begin_local_turn(world, orders, 400);
  CHECK(turn.length == 400);  // at 1000, before the order
  REQUIRE(turn.orders.orders.size() == 1);
  CHECK(turn.orders.orders[0].issuer == 3);
  CHECK(turn.report.speeds == 1);
  CHECK(world.clock().config().game_speed == 2000);
  // Nothing posted: nothing applied, and the speed stands.
  const LocalTurn quiet = begin_local_turn(world, orders, 400);
  CHECK(quiet.length == 800);
  CHECK(quiet.orders.orders.empty());
  CHECK(quiet.report.speeds == 0);
}
