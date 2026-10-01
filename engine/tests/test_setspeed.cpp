// `SetSpeed` and `GetSpeed`, from a script.
//
// What carries the reading (0x004c6ce0, 0x004c6220):
//
//   * `GetSpeed()` is the clock's speed, per mille: world state.
//   * `SetSpeed(n)` changes nothing itself. It posts a `set_speed` order
//     with the local player as issuer through the local command path
//     (`HostContext::outbox`), and the order changes the clock on the turn
//     it is applied with -- so the next `GetSpeed()` in the same turn still
//     answers the old speed.
//   * A match that fixed its speed (game `+0x230`) posts nothing, and a run
//     with no command path posts nothing either.

#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// A world, the registry every domain fills, and a context pointing at both.
struct SpeedBench {
  World world;
  script::HostRegistry registry;
  HostContext context;
  LocalOrders orders;

  SpeedBench() {
    (void)register_all_hosts(registry);
    context.world = &world;
    context.local_player = 2;
    context.outbox = &orders;
  }

  script::HostOutcome call(const char* name, std::uint16_t arity,
                           std::span<script::Value> args = {}) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name, arity);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostFn fn = registry.entry(index).fn;
    if (fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.user = &context;
    ctx.arguments = args;
    ctx.name = name;
    return fn(ctx);
  }

  script::HostOutcome set(std::int32_t speed) {
    script::Value args[] = {script::Value::integer(speed)};
    return call("SetSpeed", 1, args);
  }
  std::int32_t get() { return call("GetSpeed", 0).value.as_integer(); }
};

/// Records what is posted, for the paths `LocalOrders` does not show.
class Recorder final : public OrderOutbox {
 public:
  [[nodiscard]] bool speed_fixed() const noexcept override { return fixed; }
  void post(NetOrder order) override { posted.push_back(std::move(order)); }
  bool fixed = false;
  std::vector<NetOrder> posted;
};

}  // namespace

TEST(get_speed_is_the_clocks_speed) {
  SpeedBench b;
  CHECK(b.call("GetSpeed", 0).status == script::HostStatus::ok);
  CHECK(b.get() == 1000);
  b.world.clock().set_game_speed(1400);
  CHECK(b.get() == 1400);
}

TEST(set_speed_posts_an_order_and_changes_nothing_until_it_is_applied) {
  SpeedBench b;
  const std::uint64_t before = b.world.hashes().hash_of_hashes;
  CHECK(b.set(2000).status == script::HostStatus::ok);
  // Posted, not written: the clock, the world and GetSpeed are as they were.
  CHECK(b.get() == 1000);
  CHECK(b.world.hashes().hash_of_hashes == before);
  REQUIRE(!b.orders.empty());

  // The next turn's head applies it; that turn is still at the old speed.
  const LocalTurn turn = begin_local_turn(b.world, b.orders, 800);
  CHECK(turn.length == 800);
  REQUIRE(turn.orders.orders.size() == 1);
  const NetOrder& order = turn.orders.orders[0];
  CHECK(order.kind == NetOrderKind::set_speed);
  CHECK(order.speed == 2000);
  CHECK(order.issuer == 2);  // the local player, as 0x004c6ce0 sets it
  CHECK(b.get() == 2000);
  CHECK(begin_local_turn(b.world, b.orders, 800).length == 1600);
}

TEST(set_speed_passes_its_argument_as_asked_and_the_clamp_is_where_it_applies) {
  SpeedBench b;
  Recorder recorder;
  b.context.outbox = &recorder;
  CHECK(b.set(-5).status == script::HostStatus::ok);
  CHECK(b.set(500000).status == script::HostStatus::ok);
  REQUIRE(recorder.posted.size() == 2);
  CHECK(recorder.posted[0].speed == -5);
  CHECK(recorder.posted[1].speed == 500000);
  NetTurn turn;
  turn.orders = recorder.posted;
  turn.orders.pop_back();
  (void)apply_turn(b.world, turn);
  CHECK(b.get() == 1);
}

TEST(set_speed_posts_nothing_in_a_match_that_fixed_its_speed) {
  SpeedBench b;
  Recorder recorder;
  recorder.fixed = true;
  b.context.outbox = &recorder;
  CHECK(b.set(3000).status == script::HostStatus::ok);
  CHECK(recorder.posted.empty());
  // And `LocalOrders` carries the same flag.
  b.context.outbox = &b.orders;
  b.orders.set_speed_fixed(true);
  CHECK(b.set(3000).status == script::HostStatus::ok);
  CHECK(b.orders.empty());
  b.orders.set_speed_fixed(false);
  CHECK(b.set(3000).status == script::HostStatus::ok);
  CHECK(!b.orders.empty());
}

TEST(set_speed_with_no_command_path_is_a_quiet_no_op) {
  SpeedBench b;
  b.context.outbox = nullptr;
  CHECK(b.set(3000).status == script::HostStatus::ok);
  CHECK(b.get() == 1000);
}

TEST(set_speed_and_get_speed_refuse_without_a_world) {
  SpeedBench b;
  b.context.world = nullptr;
  CHECK(b.set(3000).status == script::HostStatus::error);
  CHECK(b.call("GetSpeed", 0).status == script::HostStatus::error);
  CHECK(b.orders.empty());
  // A non-integer speed is refused too, and posts nothing.
  b.context.world = &b.world;
  script::Value text[] = {script::Value::string("fast")};
  CHECK(b.call("SetSpeed", 1, text).status == script::HostStatus::error);
  CHECK(b.orders.empty());
}
