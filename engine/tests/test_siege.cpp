// The siege planner: sim/siege.hpp.
//
// Every number here is a transcription of a shipped class property or of the
// three gate markers `BUILDINGS\RWALLS\GATEN\GATEN.ENT.XML` carries, named
// where it is used. No game data is copied in bulk: the fixture declares the
// dozen classes the planner asks about and nothing else.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/siege.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// The Republican `GateN` entity's two type-7 markers and three type-10
/// markers, and nothing else of it: the wall axis and the opening the segment
/// routine (0x00529380) reads.
constexpr std::string_view kGateEntity = R"(<?xml version="1.0"?>
<entity name="GateN" type="vx/building" variations="1">
  <points>
    <point idx="1" type="7" x="220" y="110"/>
    <point idx="2" type="7" x="-220" y="-110"/>
    <point idx="7" type="10" x="51" y="-2"/>
    <point idx="8" type="10" x="-42" y="-33"/>
    <point idx="9" type="10" x="-1" y="-18"/>
  </points>
</entity>)";

/// `REQUIRE` for a function that returns a value.
#define REQUIRE_RETURN(expr, value)                                                       \
  do {                                                                                    \
    if (!::imperivm::test::check((expr), "REQUIRE(" #expr ")", __FILE__, __LINE__)) return value; \
  } while (0)

constexpr GameTime kNow = 1000;
constexpr std::int32_t kMapSize = 8192;

/// A walled town of player 2 besieged by player 1's army, with the systems
/// the planner reads: economy (settlements and holders), commands, heroes
/// (squads and attachment), combat (the engine's target), match (the map
/// rectangle) and movement (the obstruction grid).
struct SiegeBench {
  ClassGraph graph;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  CommandSystem commands;
  HeroSystem heroes;
  CombatSystem combat;
  MatchSystem match;
  MovementSystem movement;
  Result<Entity> gate_entity = Entity::parse(bytes_of(kGateEntity), "GateN.ent.xml");

  SiegeBench() {
    const std::string docs[] = {
        R"(<class id="Unit" cpp_class="CVXUnit"><properties maxhealth="100" damage_type="slash" damage="10"/>
             <method sig="idle" vs="data/subai/unit_idle.vs"/>
             <method sig="move" vs="data/subai/unit_move.vs"/>
             <method sig="attack" vs="data/subai/unit_attack.vs"/>
             <method sig="ai_attack_gate" vs="data/subai/unit_ai_attack_gate.vs"/>
             <method sig="build_catapult" vs="data/subai/unit_build_catapult.vs"/></class>)",
        R"(<class id="Military" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="GSwordsman" parent="Military" cpp_class="CVXUnit"><properties race="Gaul" damage="30" damage_type="slash"/></class>)",
        R"(<class id="GArcher" parent="Military" cpp_class="CVXUnit"><properties race="Gaul" damage="20" damage_type="pierce" projectile_class="Arrow"/></class>)",
        R"(<class id="GLongbow" parent="Military" cpp_class="CVXUnit"><properties race="Gaul" damage="200" damage_type="pierce" projectile_class="Arrow"/></class>)",
        R"(<class id="BSwordsman" parent="Military" cpp_class="CVXUnit"><properties race="Britain" damage="30" damage_type="slash"/></class>)",
        R"(<class id="GPeasant" parent="Unit" cpp_class="CVXUnit"><properties race="Gaul" damage="0" damage_type="none"/></class>)",
        R"(<class id="Hero" parent="Unit" cpp_class="CVXHero"><properties race="Gaul" damage="40" damage_type="slash" max_army="8"/></class>)",
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="3000" radius="150" can_be_attacked="1"/></class>)",
        R"(<class id="TownHall" parent="Building" cpp_class="CVXTownHall"><properties is_central_building="1" max_units="10"/></class>)",
        R"(<class id="Outpost" parent="Building" cpp_class="CVXTownHall"><properties is_central_building="1" max_units="10000"/></class>)",
        R"(<class id="Gate" parent="Building" cpp_class="CVXGate"><properties radius="170" maxhealth="2000"/></class>)",
        R"(<class id="RGateN" parent="Gate" cpp_class="CVXGate"/>)",
        R"(<class id="Catapult" parent="Building" cpp_class="CVXCatapult"><properties radius="50" range="800" min_range="301" damage="120" damage_type="siege" maxhealth="1000" is_central_building="1" can_be_attacked="1" max_units="10"/></class>)",
        R"(<class id="GCatapult" parent="Catapult" cpp_class="CVXCatapult"/>)",
        R"(<class id="BCatapult" parent="Catapult" cpp_class="CVXCatapult"/>)",
    };
    const char* names[] = {"unit.sc.xml",     "military.sc.xml", "gsword.sc.xml",   "garcher.sc.xml",
                           "glongbow.sc.xml", "bsword.sc.xml",   "gpeasant.sc.xml", "hero.sc.xml",
                           "building.sc.xml", "townhall.sc.xml", "outpost.sc.xml",  "gate.sc.xml",
                           "rgaten.sc.xml",   "catapult.sc.xml", "gcatapult.sc.xml", "bcatapult.sc.xml"};
    for (int i = 0; i < 16; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_command_host(registry);
    register_economy_hosts(registry);
    register_squad_host(registry);
    register_siege_host(registry);
    world.set_class_graph(&graph);
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&heroes));
    REQUIRE(world.add_system(&combat));
    combat.set_world_bound(true);  // what the session does: `reconcile` is a no-op otherwise
    REQUIRE(world.add_system(&match));
    REQUIRE(world.add_system(&movement));
    economy.start(world);
    MatchRules rules;
    rules.map_size = kMapSize;
    (void)setup_match(world, match, rules, MatchOptions{});
    movement.set_grid(ObstructionGrid(kMapSize / kCollisionCellSize, kMapSize / kCollisionCellSize));
  }

  /// A unit with a running `idle`, as every unit in play has one.
  ObjectId unit(const char* cls, Point at, PlayerId owner = 1) {
    const bool hero = std::string_view(cls) == "Hero";
    const ObjectId id = world.spawn(hero ? NativeClass::hero : NativeClass::unit, nullptr,
                                    graph.find(cls));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 100);
    heroes.register_unit(world, id);
    if (hero) heroes.register_hero(world, id);
    (void)commands.set_command(world, id, "idle", Command{});
    return id;
  }

  std::vector<ObjectId> army(const char* cls, std::size_t n, Point from, PlayerId owner = 1) {
    std::vector<ObjectId> out;
    for (std::size_t i = 0; i < n; ++i) {
      out.push_back(unit(cls, Point{from.x + static_cast<std::int32_t>(i) * 20, from.y}, owner));
    }
    return out;
  }

  struct Town {
    SettlementId id = kNoSettlement;
    ObjectId hall = kNoObject;
    ObjectId gate = kNoObject;
  };

  /// A settlement anchored on `hall_cls` at `at`, owned by `owner`, and -- when
  /// asked -- a Republican north gate 600 units east of it.
  Town town(const char* hall_cls, Point at, PlayerId owner, bool with_gate,
            SettlementKind kind = SettlementKind::stronghold) {
    Town out;
    out.hall = world.spawn(NativeClass::town_hall, nullptr, graph.find(hall_cls));
    world.set_position(out.hall, at);
    world.set_owner(out.hall, owner);
    world.set_health(out.hall, 3000);
    SettlementInit init;
    init.kind = kind;
    init.anchor = out.hall;
    init.owner = owner;
    init.max_units = 10;
    init.can_be_attacked = true;
    init.anchor_max_health = 3000;
    out.id = economy.create(world, init);
    REQUIRE_RETURN(out.id != kNoSettlement, out);
    (void)economy.add_building(world, out.id, out.hall, 3000);
    if (with_gate) {
      REQUIRE_RETURN(gate_entity.ok(), out);
      out.gate = world.spawn(NativeClass::gate, &gate_entity.value(), graph.find("RGateN"));
      world.set_position(out.gate, Point{at.x + 600, at.y});
      world.set_owner(out.gate, owner);
      world.set_health(out.gate, 2000);
      (void)economy.add_building(world, out.id, out.gate, 2000);
    }
    combat.start(world);
    return out;
  }

  SiegeReport run(const std::vector<ObjectId>& units, ObjectId target, std::int32_t cats,
                  std::int32_t state) {
    return run_siege_plan(world, units, target, cats, state, kNow);
  }

  std::vector<std::string> verbs(ObjectId id) const {
    std::vector<std::string> out;
    const CommandQueue* q = commands.find(id);
    if (q == nullptr) return out;
    for (const Command& command : q->entries) out.emplace_back(command.verb);
    return out;
  }
  ObjectId target_of(ObjectId id, std::size_t index) const {
    const CommandQueue* q = commands.find(id);
    if (q == nullptr || index >= q->entries.size()) return kNoObject;
    return q->entries[index].arg_kind == CommandArgKind::object ? q->entries[index].object
                                                                 : kNoObject;
  }
  Point point_of(ObjectId id, std::size_t index) const {
    const CommandQueue* q = commands.find(id);
    if (q == nullptr || index >= q->entries.size()) return Point{-1, -1};
    return q->entries[index].arg_kind == CommandArgKind::point ? q->entries[index].point
                                                               : Point{-1, -1};
  }

  /// Every catapult in the world, ascending id.
  std::vector<ObjectId> catapults() const {
    std::vector<ObjectId> out;
    const ClassIndex base = graph.find("Catapult");
    for (const WorldObject& slot : world.objects()) {
      if (slot.internal != InternalKind::none) continue;
      if (world.class_is_a(slot.id, base)) out.push_back(slot.id);
    }
    return out;
  }
};

std::int64_t distance_between(Point a, Point b) {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  return isqrt(dx * dx + dy * dy);
}

}  // namespace

/// **A gate siege builds one engine on the wall's outer side, crews it with
/// the nearest ten swordsmen, and stands everyone else back behind it.**
/// 0x004383c0's split, 0x00448310's nine sites, 0x00435ca0's nearest-first
/// crews, 0x00436490's orders and regrouping, and the stand-back branch that
/// ten crews select.
TEST(siege_of_a_gate_places_an_engine_crews_it_and_stands_the_rest_back) {
  SiegeBench b;
  const SiegeBench::Town town = b.town("TownHall", Point{4400, 5000}, 2, true);
  REQUIRE(town.gate != kNoObject);

  // The GateN axis at (5000, 5000): markers (220,110)/(-220,-110), shifted by
  // the type-10 mean (2,-17) less its foot on the axis (-5,-2) -- so from
  // (5227,5095) to (4787,4875); the outward normal for a hall to the west is
  // (301,-602) at radius 672. Site 0 is therefore (5528, 4493), and the first
  // unit stands nearest it.
  const Point site0{5528, 4493};
  std::vector<ObjectId> units = b.army("GSwordsman", 20, Point{5600, 4400});
  const std::vector<ObjectId> archers = b.army("GArcher", 4, Point{6200, 4400});
  units.insert(units.end(), archers.begin(), archers.end());
  // Two of the crews follow a hero who is not in the list: a crew leaves its
  // hero before it is regrouped (0x005db190, `CVXUnit::DetachHero`).
  const ObjectId leader = b.unit("Hero", Point{5600, 4300});
  REQUIRE(b.heroes.attach(b.world, units[0], leader));
  REQUIRE(b.heroes.attach(b.world, units[1], leader));

  const SiegeReport report = b.run(units, town.gate, 24 / 15, 7);
  CHECK(report.wanted_catapults);
  CHECK(report.placed == 1);
  CHECK(report.crews == 10);
  CHECK(report.crews_inside == 0);
  CHECK(!report.attacked);

  const std::vector<ObjectId> engines = b.catapults();
  REQUIRE(engines.size() == 1);
  const WorldObject* engine = b.world.find(engines[0]);
  REQUIRE(engine != nullptr);
  CHECK(engine->class_index == b.graph.find("GCatapult"));
  CHECK(engine->state.owner == 1);
  CHECK(engine->state.health == 1);
  CHECK(engine->state.position == site0);
  // Aimed at the gate, in the slot `Catapult::GetCurrentTarget` reads.
  REQUIRE(b.combat.find(engines[0]) != nullptr);
  CHECK(b.combat.find(engines[0])->target == town.gate);
  // With a settlement of its own whose holder caps the crew at `max_units`.
  const Settlement* own = b.economy.settlements().for_object(engine->settlement);
  REQUIRE(own != nullptr);
  CHECK(own->anchor == engines[0]);
  CHECK(own->holder.max_units == 10);

  // The ten swordsmen nearest the engine build it; they left their `idle`.
  std::vector<ObjectId> crews;
  std::vector<ObjectId> rest;
  for (std::size_t i = 0; i < 20; ++i) {
    (distance_between(b.world.resolve_position(units[i]), site0) <
             distance_between(b.world.resolve_position(units[10]), site0)
         ? crews
         : rest)
        .push_back(units[i]);
  }
  CHECK(crews.size() == 10);
  CHECK(b.heroes.unit(units[0]) != nullptr && b.heroes.unit(units[0])->hero == kNoObject);
  CHECK(b.heroes.unit(units[1]) != nullptr && b.heroes.unit(units[1])->hero == kNoObject);
  for (const ObjectId id : crews) {
    CHECK(b.verbs(id) == std::vector<std::string>{"build_catapult"});
    CHECK(b.target_of(id, 0) == engines[0]);
    // Regrouped: a squad of its own carrying the siege state, stamped now.
    const Squad* squad = b.heroes.squads().find(b.heroes.squads().squad_of(id));
    REQUIRE(squad != nullptr);
    CHECK(squad->members.size() == 1);
    CHECK(squad->state == 7);
    CHECK(squad->state_time == kNow);
  }
  // Everyone else -- the ten further swordsmen and the four archers -- stands
  // back: 200 then 400 units beyond the engine along the ray from the gate.
  const auto push = [&](Point from, std::int32_t by) {
    const Point gate_at = b.world.find(town.gate)->state.position;
    const std::int64_t dx = static_cast<std::int64_t>(from.x) - gate_at.x;
    const std::int64_t dy = static_cast<std::int64_t>(from.y) - gate_at.y;
    const std::int64_t len = isqrt(dx * dx + dy * dy);
    return Point{static_cast<std::int32_t>(from.x + dx * by / len),
                 static_cast<std::int32_t>(from.y + dy * by / len)};
  };
  const Point stand = push(push(site0, 200), 400);
  for (const ObjectId id : rest) {
    CHECK(b.verbs(id) == (std::vector<std::string>{"move", "idle"}));
    CHECK(b.point_of(id, 0) == stand);
    CHECK(!b.heroes.squads().squad_of(id).valid());
  }
  for (const ObjectId id : archers) {
    CHECK(b.verbs(id) == (std::vector<std::string>{"move", "idle"}));
    CHECK(b.point_of(id, 0) == stand);
  }

  // An army at the axis's other end takes site 8, (5088, 4273): the nine
  // sites run the whole wall, not a corner of it.
  SiegeBench c;
  const SiegeBench::Town far = c.town("TownHall", Point{4400, 5000}, 2, true);
  const std::vector<ObjectId> west = c.army("GSwordsman", 15, Point{5060, 4250});
  CHECK(c.run(west, far.gate, 1, 7).placed == 1);
  REQUIRE(c.catapults().size() == 1);
  CHECK(c.world.find(c.catapults()[0])->state.position == (Point{5088, 4273}));
}

/// **No site, no engine, and a melee majority charges.** With the grid blocked
/// everywhere the nine gate sites all fail, the crews rejoin the attackers,
/// five swordsmen outnumber one archer, and everyone gets `ai_attack_gate` at
/// the gate -- except whoever stands behind the first hero ordered.
TEST(siege_without_a_site_sends_a_melee_majority_at_the_gate_and_stops_at_a_hero) {
  SiegeBench b;
  const SiegeBench::Town town = b.town("TownHall", Point{4400, 5000}, 2, true);
  REQUIRE(town.gate != kNoObject);
  ObstructionGrid blocked(kMapSize / kCollisionCellSize, kMapSize / kCollisionCellSize);
  for (std::int32_t y = 0; y < blocked.height(); ++y) {
    for (std::int32_t x = 0; x < blocked.width(); ++x) blocked.set_cell(x, y, true);
  }
  b.movement.set_grid(std::move(blocked));

  std::vector<ObjectId> units = b.army("GSwordsman", 5, Point{5600, 4400});
  units.push_back(b.unit("GArcher", Point{5700, 4400}));
  units.push_back(b.unit("Hero", Point{5800, 4400}));
  const ObjectId behind = b.unit("GSwordsman", Point{5900, 4400});
  units.push_back(behind);

  const SiegeReport report = b.run(units, town.gate, 1, 7);
  CHECK(report.wanted_catapults);
  CHECK(report.placed == 0);
  CHECK(report.crews == 0);
  CHECK(report.attacked);
  CHECK(b.catapults().empty());
  // The attackers' order is the list's non-crews first -- archer, hero, the
  // swordsman behind -- and then the crews that found no engine (0x00436490
  // appends them). So the archer and the hero are ordered, the hero ends the
  // loop, and the five crews and the swordsman behind keep their `idle`.
  CHECK(b.verbs(units[5]) == std::vector<std::string>{"ai_attack_gate"});
  CHECK(b.target_of(units[5], 0) == town.gate);
  CHECK(b.verbs(units[6]) == std::vector<std::string>{"ai_attack_gate"});
  for (std::size_t i = 0; i < 5; ++i) CHECK(b.verbs(units[i]) == std::vector<std::string>{"idle"});
  CHECK(b.verbs(behind) == std::vector<std::string>{"idle"});

  // A tie is not a majority: two swordsmen and two archers stand back, and
  // with no site to stand back from nobody is ordered anywhere.
  SiegeBench c;
  const SiegeBench::Town keep = c.town("TownHall", Point{4400, 5000}, 2, true);
  ObstructionGrid wall(kMapSize / kCollisionCellSize, kMapSize / kCollisionCellSize);
  for (std::int32_t y = 0; y < wall.height(); ++y) {
    for (std::int32_t x = 0; x < wall.width(); ++x) wall.set_cell(x, y, true);
  }
  c.movement.set_grid(std::move(wall));
  std::vector<ObjectId> even = c.army("GSwordsman", 2, Point{5600, 4400});
  const std::vector<ObjectId> two = c.army("GArcher", 2, Point{5700, 4400});
  even.insert(even.end(), two.begin(), two.end());
  const SiegeReport tie = c.run(even, keep.gate, 1, 7);
  CHECK(!tie.attacked);
  for (const ObjectId id : even) CHECK(c.verbs(id) == std::vector<std::string>{"idle"});
}

/// **Enough ranged damage makes the engines pointless.** Two longbows at 200
/// damage are 400, and five times that is exactly the gate's 2,000 health --
/// not below it -- so nothing is built and the army attacks; `attack`, not
/// `ai_attack_gate`, when the target is the hall.
TEST(siege_skips_the_engines_when_the_attackers_can_bring_the_target_down) {
  SiegeBench b;
  const SiegeBench::Town town = b.town("TownHall", Point{4400, 5000}, 2, true);
  REQUIRE(town.gate != kNoObject);
  std::vector<ObjectId> units = b.army("GSwordsman", 3, Point{5600, 4400});
  const std::vector<ObjectId> bows = b.army("GLongbow", 2, Point{5700, 4400});
  units.insert(units.end(), bows.begin(), bows.end());

  const SiegeReport report = b.run(units, town.gate, 1, 7);
  CHECK(!report.wanted_catapults);
  CHECK(report.placed == 0);
  CHECK(report.attacked);
  CHECK(b.catapults().empty());
  for (const ObjectId id : units) CHECK(b.verbs(id) == std::vector<std::string>{"ai_attack_gate"});

  // The hall as target: `attack`. The town's holder is empty, so the central
  // building wants no engines at all.
  SiegeBench c;
  const SiegeBench::Town keep = c.town("TownHall", Point{4400, 5000}, 2, false);
  const std::vector<ObjectId> swords = c.army("GSwordsman", 6, Point{5600, 4400});
  const SiegeReport hall = c.run(swords, keep.hall, 1, 7);
  CHECK(!hall.wanted_catapults);
  CHECK(hall.attacked);
  for (const ObjectId id : swords) {
    CHECK(c.verbs(id) == std::vector<std::string>{"attack"});
    CHECK(c.target_of(id, 0) == keep.hall);
  }

  // The central building's five-round skirmish (0x00438791). A garrisoned
  // outpost wants engines, but its hall stands at half health, so of the four
  // longbows' 800 a round, half lands on the garrison -- and the one defender
  // inside, at 100, is gone in the first round. Nothing is built; the fifteen
  // swordsmen who would have crewed attack with the bows.
  SiegeBench d;
  const SiegeBench::Town post = d.town("Outpost", Point{4000, 4000}, 2, false, SettlementKind::outpost);
  REQUIRE(post.id != kNoSettlement);
  REQUIRE(d.economy.garrison_add(post.id, d.unit("GSwordsman", Point{4000, 4000}, 2)));
  d.world.set_health(post.hall, 1500);
  std::vector<ObjectId> host = d.army("GSwordsman", 15, Point{4700, 4000});
  const std::vector<ObjectId> longbows = d.army("GLongbow", 4, Point{4900, 4000});
  host.insert(host.end(), longbows.begin(), longbows.end());
  const SiegeReport skirmish = d.run(host, post.hall, 2, 7);
  CHECK(!skirmish.wanted_catapults);
  CHECK(skirmish.attacked);
  CHECK(d.catapults().empty());
  for (const ObjectId id : host) CHECK(d.verbs(id) == std::vector<std::string>{"attack"});
  // A sound hall against one longbow: the first round lands wholly on the
  // hall, and as it weakens the garrison's share grows -- 14, 26, 38, 49 over
  // the next four rounds, 127 in all -- so two defenders at 200 are still
  // standing after the fifth and the engines stay wanted.
  SiegeBench e;
  const SiegeBench::Town sound = e.town("Outpost", Point{4000, 4000}, 2, false, SettlementKind::outpost);
  REQUIRE(e.economy.garrison_add(sound.id, e.unit("GSwordsman", Point{4000, 4000}, 2)));
  REQUIRE(e.economy.garrison_add(sound.id, e.unit("GSwordsman", Point{4000, 4000}, 2)));
  std::vector<ObjectId> again = e.army("GSwordsman", 15, Point{4700, 4000});
  again.push_back(e.unit("GLongbow", Point{4900, 4000}));
  const SiegeReport held = e.run(again, sound.hall, 2, 7);
  CHECK(held.wanted_catapults);
  CHECK(held.placed == 2);
}

/// **An outpost is besieged from a circle.** With a garrison inside the
/// central building wants engines, the sites are eight points at radius 672
/// around it, and a second engine cannot take the first one's site.
TEST(siege_of_an_outpost_uses_the_circle_and_never_stacks_two_engines) {
  SiegeBench b;
  const SiegeBench::Town post = b.town("Outpost", Point{4000, 4000}, 2, false, SettlementKind::outpost);
  REQUIRE(post.id != kNoSettlement);
  const ObjectId defender = b.unit("GSwordsman", Point{4000, 4000}, 2);
  REQUIRE(b.economy.garrison_add(post.id, defender));

  // Thirty swordsmen east of the outpost; the nearest site is (4672, 4000).
  const std::vector<ObjectId> units = b.army("GSwordsman", 30, Point{4700, 4000});
  const SiegeReport report = b.run(units, post.hall, 2, 7);
  CHECK(report.wanted_catapults);
  CHECK(report.placed == 2);
  CHECK(report.crews == 20);
  CHECK(!report.attacked);

  const std::vector<ObjectId> engines = b.catapults();
  REQUIRE(engines.size() == 2);
  const Point first = b.world.find(engines[0])->state.position;
  const Point second = b.world.find(engines[1])->state.position;
  CHECK(first == (Point{4672, 4000}));
  CHECK(second != first);
  // The second is the next site out from the first unit: the diagonal
  // (4000 + 475, 4000 - 475) or its mirror, both 475 from the axis.
  CHECK((second == Point{4475, 3525} || second == Point{4475, 4475}));
  for (const ObjectId id : engines) {
    REQUIRE(b.combat.find(id) != nullptr);
    CHECK(b.combat.find(id)->target == post.hall);
  }
  // The ten unassigned stand back from the second engine's site.
  std::size_t standing = 0;
  for (const ObjectId id : units) {
    if (b.verbs(id) == (std::vector<std::string>{"move", "idle"})) ++standing;
  }
  CHECK(standing == 10);
}

/// Playtest #19's two leads, on an engine placed inside a garrisoned town.
/// `CATAPULT_IDLE.VS` builds only while `.settlement.UnitsCount()` reaches
/// `CatapultBuildUnits`, so the engine's `.settlement` must be its own
/// single-building settlement (`[cat+0x148]`, read by `Building::settlement`,
/// 0x004dcdd0) and not the town it stands in, and its count (the holder's own
/// list, `Settlement::UnitsCount`, 0x005c1d80) must hold only units that have
/// entered: `UNIT_BUILD_CATAPULT.VS`'s `SetEntering(true)` is one bit
/// (0x005d8240) and the builder counts only after `AddUnit`.
TEST(a_new_engine_in_a_town_counts_its_own_crew_and_only_those_inside) {
  SiegeBench b;
  const SiegeBench::Town town = b.town("TownHall", Point{4400, 5000}, 1, false);
  for (int i = 0; i < 3; ++i) {
    REQUIRE(b.economy.garrison_add(town.id, b.unit("GSwordsman", Point{4400, 5000})));
  }
  // Placed beside the town hall, by player 1 (2 to a script).
  const ObjectId engine = place_catapult(b.world, b.graph.find("GCatapult"), Point{4500, 5000}, 2);
  REQUIRE(engine != kNoObject);
  CHECK(b.world.find(engine)->state.health == 1);

  HostContext context;
  context.world = &b.world;
  const auto call = [&](const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = b.registry.find(script::CallKind::member, name,
                                                static_cast<std::uint16_t>(args.size() - 1));
    REQUIRE_RETURN(index != script::kUnresolvedHost, script::HostOutcome::failed("undeclared"));
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return b.registry.entry(index).fn(ctx);
  };
  const script::HostOutcome own = call("settlement", {script::Value::object(kTypeObj, engine)});
  REQUIRE(own.value.is_object());
  const Settlement* town_row = b.economy.settlements().find(town.id);
  REQUIRE(town_row != nullptr);
  CHECK(own.value.as_object().id != town_row->object);
  const Settlement* mine = b.economy.settlements().for_object(own.value.as_object().id);
  REQUIRE(mine != nullptr);
  CHECK(mine->anchor == engine);
  const auto inside = [&] {
    const script::HostOutcome out = call("UnitsCount", {own.value});
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(town_row->holder.count() == 3);
  CHECK(inside() == 0);

  // A builder on its way: entering, and not counted.
  const ObjectId builder = b.unit("GSwordsman", Point{5200, 5000});
  CHECK(call("SetEntering", {script::Value::object(kTypeObj, builder), script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(b.world.find(builder)->state.flags.entering);
  CHECK(inside() == 0);
  // At the door: `AddUnit`, and now it counts.
  const script::HostOutcome added =
      call("AddUnit", {own.value, script::Value::object(kTypeObj, builder)});
  CHECK(added.value.truthy_scalar());
  CHECK(inside() == 1);
  CHECK(b.economy.settlements().find(town.id)->holder.count() == 3);
}

/// **An engine already aimed at the target is crewed before any is built.**
/// The settlement roll walk at 0x00438835: the owner's catapult whose target
/// is this target takes the nearest crews up to its holder's cap, its site is
/// the rally point, and with the cap of one reached nothing new is placed.
TEST(siege_fills_an_existing_engine_first) {
  SiegeBench b;
  const SiegeBench::Town town = b.town("TownHall", Point{4400, 5000}, 2, true);
  REQUIRE(town.gate != kNoObject);
  // The script-side player, 1-based: owner 1 is player 2.
  const ObjectId engine = place_catapult(b.world, b.graph.find("GCatapult"), Point{5300, 4600}, 2);
  REQUIRE(engine != kNoObject);
  b.combat.start(b.world);
  REQUIRE(b.combat.order_attack(engine, town.gate));
  // One crewman is already inside: it counts toward the ten and takes a slot.
  const Settlement* own = b.economy.settlements().for_object(b.world.find(engine)->settlement);
  REQUIRE(own != nullptr);
  REQUIRE(b.economy.garrison_add(own->id, b.unit("GSwordsman", Point{5300, 4600})));

  const std::vector<ObjectId> units = b.army("GSwordsman", 12, Point{5600, 4400});
  const SiegeReport report = b.run(units, town.gate, 1, 7);
  CHECK(report.placed == 0);
  CHECK(report.crews == 9);
  CHECK(report.crews_inside == 1);
  CHECK(!report.attacked);
  CHECK(b.catapults().size() == 1);
  std::size_t building = 0;
  for (const ObjectId id : units) {
    if (b.verbs(id) == std::vector<std::string>{"build_catapult"}) {
      ++building;
      CHECK(b.target_of(id, 0) == engine);
    }
  }
  CHECK(building == 9);
  // Nine crews and one inside: the three left over stand back from the
  // existing engine rather than charge.
  const Point gate_at = b.world.find(town.gate)->state.position;
  for (const ObjectId id : units) {
    if (b.verbs(id) != (std::vector<std::string>{"move", "idle"})) continue;
    const Point stand = b.point_of(id, 0);
    CHECK(distance_between(stand, gate_at) > distance_between(Point{5300, 4600}, gate_at));
  }
}

/// **Britain and Germany get one engine, whatever was asked.** 0x004386d5:
/// a majority race of 5 or 7 writes 1 over the cap.
TEST(siege_caps_britons_at_one_engine) {
  SiegeBench b;
  const SiegeBench::Town post = b.town("Outpost", Point{4000, 4000}, 2, false, SettlementKind::outpost);
  REQUIRE(b.economy.garrison_add(post.id, b.unit("GSwordsman", Point{4000, 4000}, 2)));
  const std::vector<ObjectId> units = b.army("BSwordsman", 30, Point{4700, 4000});
  const SiegeReport report = b.run(units, post.hall, 3, 7);
  CHECK(report.placed == 1);
  CHECK(report.crews == 10);
  const std::vector<ObjectId> engines = b.catapults();
  REQUIRE(engines.size() == 1);
  CHECK(b.world.find(engines[0])->class_index == b.graph.find("BCatapult"));
}

/// **`Squad::Siege` writes the state, refuses `SF_NOAI`, and its crews leave
/// the squad** -- which is what `GS_CAPTURE.VS` measures as
/// `ol.count - squad.Units.count`. `ObjList::Siege` is the same body on a
/// list.
TEST(squad_siege_host_writes_the_state_and_moves_its_crews_out) {
  SiegeBench b;
  const SiegeBench::Town town = b.town("TownHall", Point{4400, 5000}, 2, true);
  REQUIRE(town.gate != kNoObject);
  const std::vector<ObjectId> units = b.army("GSwordsman", 12, Point{5600, 4400});
  const SquadKey key = b.heroes.squads().create(1);
  for (const ObjectId id : units) {
    REQUIRE(b.heroes.squads().join(key, id));
    b.heroes.register_unit(b.world, id).squad = key;
  }

  HostContext context;
  context.world = &b.world;
  const auto call = [&](std::vector<script::Value> args, std::uint16_t arity) {
    const std::uint32_t index = b.registry.find(script::CallKind::member, "Siege", arity);
    REQUIRE_RETURN(index != script::kUnresolvedHost, script::HostOutcome::failed("undeclared"));
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Siege";
    ctx.kind = script::CallKind::member;
    return b.registry.entry(index).fn(ctx);
  };
  const script::Value gate = script::Value::object(kTypeObj, town.gate);

  // Refused under SF_NOAI: nothing written, nothing ordered.
  b.heroes.squads().find(key)->flags = kSquadFlagNoAi;
  script::HostOutcome out = call({pack_squad(key), gate, script::Value::integer(1),
                                  script::Value::integer(7), script::Value::integer(8)},
                                 4);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(b.heroes.squads().find(key)->state == 0);
  CHECK(b.catapults().empty());

  b.heroes.squads().find(key)->flags = 0;
  out = call({pack_squad(key), gate, script::Value::integer(1), script::Value::integer(7),
              script::Value::integer(8)},
             4);
  CHECK(out.status == script::HostStatus::ok);
  const Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->state == 8);
  CHECK(squad->state_time == b.world.time());
  // Ten crews left for squads of their own; two remain.
  CHECK(squad->members.size() == 2);
  CHECK(b.catapults().size() == 1);
  std::size_t crews = 0;
  for (const ObjectId id : units) {
    const Squad* own = b.heroes.squads().find(b.heroes.squads().squad_of(id));
    if (own != nullptr && own->key != key) {
      ++crews;
      CHECK(own->state == 7);
    }
  }
  CHECK(crews == 10);

  // The list form: an unresolved target is void and orders nothing.
  SiegeBench c;
  const SiegeBench::Town other = c.town("TownHall", Point{4400, 5000}, 2, true);
  const std::vector<ObjectId> more = c.army("GSwordsman", 3, Point{5600, 4400});
  HostContext other_context;
  other_context.world = &c.world;
  ObjListPool& pool = objlist_pool_of(c.world);
  const ObjListId list = pool.acquire_temporary(1);
  *pool.mutable_items(list) = more;
  const std::uint32_t index = c.registry.find(script::CallKind::member, "Siege", 3);
  REQUIRE(index != script::kUnresolvedHost);
  std::vector<script::Value> args{make_objlist_value(list), script::Value::object(kTypeObj, 60000),
                                  script::Value::integer(1), script::Value::integer(0)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &other_context;
  ctx.name = "Siege";
  ctx.kind = script::CallKind::member;
  CHECK(c.registry.entry(index).fn(ctx).status == script::HostStatus::ok);
  for (const ObjectId id : more) CHECK(c.verbs(id) == std::vector<std::string>{"idle"});
  args[1] = script::Value::object(kTypeObj, other.gate);
  ctx.arguments = args;
  CHECK(c.registry.entry(index).fn(ctx).status == script::HostStatus::ok);
  CHECK(!c.catapults().empty());
}
