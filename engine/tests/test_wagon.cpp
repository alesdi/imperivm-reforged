// Wagon tests: the `CVXWagon` as `gbr.exe` keeps it -- its loyalty and the
// two timers a capture runs on. `docs/formats/wagon.md` is the model.
//
// No game data. The constants are `DATA\CONST.INI`'s `[GamePlay]` wagon keys
// as `EconomyRules` holds them, and the classes are written from
// `docs/formats/sc-xml.md`: a `Trader` with `Unit`'s 500 sight, as every
// shipped caravan inherits it.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A caravan and the soldiers who guard and threaten it. The guard circle is
/// the caravan's sight times 160 / 100: 800.
ClassGraph wagon_graph() {
  ClassGraph graph;
  for (const char* xml : {
           R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
           R"(<class id="Military" parent="Object" cpp_class="CVXUnit">
                <properties maxhealth="100" radius="10" sight="400"/></class>)",
           R"(<class id="Swordsman" parent="Military" cpp_class="CVXUnit"/>)",
           R"(<class id="Sentry" parent="Military" cpp_class="CVXUnit"/>)",
           R"(<class id="RamUnit" parent="Military" cpp_class="CVXUnit"/>)",
           R"(<class id="Peasant" parent="Object" cpp_class="CVXUnit">
                <properties maxhealth="50" radius="10" sight="300"/></class>)",
           R"(<class id="Trader" parent="Object" cpp_class="CVXWagon">
                <properties maxhealth="400" radius="40" sight="500" max_load="1000"/></class>)",
       }) {
    graph.add(bytes_of(xml), "test_wagon.cpp");
  }
  graph.link();
  return graph;
}

struct WagonBench {
  ClassGraph graph = wagon_graph();
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  ObjectId wagon = kNoObject;

  WagonBench() {
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&graph);
    REQUIRE(world.add_system(&economy));
    wagon = world.spawn(NativeClass::wagon, nullptr, graph.find("Trader"));
    world.set_position(wagon, Point{5000, 5000});
    world.set_owner(wagon, 1);
    world.set_health(wagon, 400);
    world.start();
  }

  ObjectId place(const char* cls, PlayerId owner, Point at, std::int32_t health = 100) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(cls));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, health);
    return id;
  }

  /// In 100 ms turns, the grain the timers are tested at.
  void play(std::int32_t ms) {
    for (std::int32_t done = 0; done < ms; done += 100) {
      const std::int32_t turn[] = {100};
      world.advance(std::span<const std::int32_t>(turn));
    }
  }

  /// A member call at the script instant `now` -- by default the world's,
  /// where a script resumed at the end of the turn stands.
  script::HostOutcome call(const char* name, std::uint16_t arity,
                           std::vector<script::Value> args, std::int64_t now = -1) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    HostContext state;
    state.world = &world;
    ctx.arguments = args;
    ctx.user = &state;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    ctx.now = now < 0 ? static_cast<std::int64_t>(world.time()) : now;
    return entry.fn(ctx);
  }

  [[nodiscard]] script::Value handle() const { return script::Value::object(kTypeObj, wagon); }

  std::int32_t loyalty() {
    const script::HostOutcome out = call("loyalty", 0, {handle()});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  }
  [[nodiscard]] std::int32_t tenths() const { return economy.wagon_loyalty_tenths(wagon); }
  void set_loyalty(std::int32_t v) {
    CHECK(call("SetLoyalty", 1, {handle(), script::Value::integer(v)}).status ==
          script::HostStatus::ok);
  }
  void taunt(std::int32_t n = 1, std::int64_t now = -1) {
    CHECK(call("DecreaseLoyalty", 1, {handle(), script::Value::integer(n)}, now).status ==
          script::HostStatus::ok);
  }
  [[nodiscard]] const WagonLoyalty* row() const { return economy.find_wagon_loyalty(wagon); }
};

}  // namespace

/// The constructed state (0x005eae80): `LoyaltyWagonsInitial` = 100 tenths,
/// so `w.loyalty` is **10, not 0** -- which is what keeps `UNIT_CAPTURE.VS`'s
/// `if (w.loyalty == 0) w.SetPlayer(.player)` from taking a caravan the moment
/// a soldier reaches it. `SetLoyalty` (0x005ec100) stores 0..100 times ten
/// and refuses anything else; neither it nor a refused `DecreaseLoyalty`
/// (0x005ec160, negative) starts a capture, so nothing moves however long the
/// clock runs.
TEST(a_wagon_starts_at_its_initial_loyalty_and_only_a_taunt_starts_a_capture) {
  WagonBench b;
  b.place("Swordsman", 2, Point{5100, 5000});  // an enemy beside it throughout
  CHECK(b.loyalty() == 10);
  CHECK(b.row() == nullptr);

  b.set_loyalty(101);
  b.set_loyalty(-1);
  CHECK(b.loyalty() == 10);
  CHECK(b.row() == nullptr);
  b.set_loyalty(40);
  CHECK(b.loyalty() == 40);
  b.taunt(-1);
  REQUIRE(b.row() != nullptr);
  CHECK(!b.row()->capturing);
  CHECK(b.row()->pending == 0);
  b.play(5000);
  CHECK(b.loyalty() == 40);
}

/// Timer 10 (0x005eb100), every `WagonLoyaltyCapInterval` = 500 from the
/// first taunt: the pending change, clamped to `10 * WagonLoyaltyChangeCap`
/// = 50 tenths either way, lands, and **the rest is dropped** with the pending
/// change. Loyalty stops at 0 however hard it is taunted -- and at
/// `LoyaltyWagonsInitial`, not 1,000, on the way back up.
TEST(a_wagons_loyalty_moves_at_most_five_points_a_half_second) {
  WagonBench b;
  b.place("Swordsman", 2, Point{5100, 5000});
  b.taunt(7);
  REQUIRE(b.row() != nullptr);
  CHECK(b.row()->capturing);
  CHECK(b.row()->pending == -70);
  b.play(400);
  CHECK(b.loyalty() == 10);  // nothing lands before the window closes
  b.play(100);
  CHECK(b.loyalty() == 5);
  CHECK(b.row()->pending == 0);
  b.play(500);
  CHECK(b.loyalty() == 5);  // the 20 tenths over the cap are gone

  for (int i = 0; i < 6; ++i) b.taunt(1);
  b.play(1000);
  CHECK(b.loyalty() == 0);
  b.taunt(100);
  b.play(500);
  CHECK(b.tenths() == 0);

  // Set above the initial value mid-capture, it comes back down to it at the
  // next window.
  b.set_loyalty(60);
  CHECK(b.loyalty() == 60);
  b.play(500);
  CHECK(b.loyalty() == 10);
}

/// Timer 9 (0x005ec410), every `WagonLoyaltyInterval` = 1000: each guard --
/// a live `Military` heir of the wagon's **own** player within
/// `sight * 160 / 100` = 800, hidden or not, a sentry or not -- adds
/// `WagonLoyaltyIncreasePerUnit` = 10 tenths to the pending change. An ally's
/// soldier, one at 810, a dead one and a peasant add nothing.
TEST(a_wagon_under_capture_is_held_by_its_own_soldiers_within_eight_hundred) {
  WagonBench b;
  b.place("Swordsman", 2, Point{5100, 5000});  // the capturer
  b.world.players().set(1, 3, Relation::ceasefire, true);
  b.world.players().set(3, 1, Relation::ceasefire, true);
  b.place("Swordsman", 1, Point{5000, 5790});
  const ObjectId hidden = b.place("Swordsman", 1, Point{4900, 5000});
  b.world.find(hidden)->state.flags.hidden = true;
  b.place("Sentry", 1, Point{5000, 4600});
  b.place("Swordsman", 1, Point{5810, 5000});     // just outside
  b.place("Swordsman", 3, Point{5050, 5050});     // an ally's
  b.place("Swordsman", 1, Point{5050, 4950}, 0);  // dead
  b.place("Peasant", 1, Point{4950, 5050}, 50);   // no soldier

  b.set_loyalty(5);
  b.taunt(1);
  b.play(500);
  CHECK(b.tenths() == 40);
  b.play(500);  // three guards: +30 at the count, landed in the same window
  CHECK(b.tenths() == 70);
  b.play(1000);  // +30 more, stopped at the initial 100
  CHECK(b.tenths() == 100);
  REQUIRE(b.row() != nullptr);
  CHECK(b.row()->capturing);
}

/// No threat at the count -- no soldier of a player the owner is at war with
/// within the circle; a `Sentry`, a `RamUnit` and a hidden one do not count,
/// nor one whose player the owner keeps a truce with though it keeps none
/// back -- and the capture is over: loyalty back at the initial 100, the flag
/// down, both timers killed. **Timer 9 runs before timer 10 on the
/// millisecond they share**, so the cap tick due with it never lands and the
/// pending change survives into the next capture.
TEST(a_wagons_capture_ends_when_no_enemy_stands_by_it) {
  WagonBench b;
  const ObjectId capturer = b.place("Swordsman", 2, Point{5100, 5000});
  b.taunt(1);
  b.play(500);
  CHECK(b.loyalty() == 9);
  b.taunt(1);  // -10 pending, due at the 1,000 window
  b.world.set_position(capturer, Point{5900, 5000});
  b.play(500);
  REQUIRE(b.row() != nullptr);
  CHECK(!b.row()->capturing);
  CHECK(b.row()->gain_remaining == 0);
  CHECK(b.row()->cap_remaining == 0);
  CHECK(b.loyalty() == 10);
  CHECK(b.row()->pending == -10);

  // None of these is a threat.
  b.place("Sentry", 2, Point{5100, 5000});
  b.place("RamUnit", 2, Point{5000, 5100});
  const ObjectId hidden = b.place("Swordsman", 2, Point{4900, 5000});
  b.world.find(hidden)->state.flags.hidden = true;
  b.world.players().set(1, 3, Relation::ceasefire, true);
  b.place("Swordsman", 3, Point{5000, 4900});
  b.taunt(1);  // -20 pending now, the old -10 with it
  b.play(500);
  CHECK(b.loyalty() == 8);
  b.play(500);
  CHECK(!b.row()->capturing);
  CHECK(b.loyalty() == 10);
}

/// A script calls `DecreaseLoyalty` at its own instant, inside a turn the
/// economy has already stepped to its end; the timers are armed from that
/// instant, so the first window closes 500 after the call and not 500 after
/// the turn.
TEST(a_wagons_capture_timers_count_from_the_taunt_not_from_the_turn) {
  WagonBench b;
  b.place("Swordsman", 2, Point{5100, 5000});
  b.play(1000);
  b.taunt(1, static_cast<std::int64_t>(b.world.time()) - 300);
  REQUIRE(b.row() != nullptr);
  CHECK(b.row()->cap_remaining == 200);
  CHECK(b.row()->gain_remaining == 700);
  b.play(200);
  CHECK(b.loyalty() == 9);
}

/// A wagon that leaves the world takes its row with it at the next step.
TEST(a_wagons_loyalty_row_goes_with_the_wagon) {
  WagonBench b;
  b.set_loyalty(30);
  REQUIRE(b.row() != nullptr);
  REQUIRE(b.world.despawn(b.wagon));
  b.play(100);
  CHECK(b.row() == nullptr);
  CHECK(b.economy.wagon_loyalties().empty());
}
