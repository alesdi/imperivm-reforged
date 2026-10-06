// The saved game: the world's own round trip, the section container, and the
// property that makes the whole thing checkable -- **loading a save reproduces
// the simulation exactly**.
//
// See include/imperivm/core/sim/save.hpp and docs/formats/save.md.
//
// The last third of this file is the part that matters. Byte-level round-trip
// tests prove the encoder and the decoder agree with each other, which is not
// the same thing as proving the save contains the game: a field nobody encodes
// round-trips perfectly, as a zero. So the real check runs the simulation N
// turns, saves, loads into a *fresh* world, runs M more turns on both, and
// requires the per-turn hash sequences to agree under the conformance harness.
// A field left out shows up there as a named channel at a named turn.
//
// And because a check that has never been seen to fail is not a check, each of
// those comes with a deliberately broken twin: a restore that skips the
// system's state, one that skips the RNG, one that skips the clock. Every one
// of them must be caught, and the test asserts that it is.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;

// --------------------------------------------------------------------------
// a system that has state worth losing
// --------------------------------------------------------------------------

/// A system with exactly the shape the real ones have: an accumulator over
/// elapsed game time, a cursor into the object table, and a draw from the
/// world's RNG.
///
/// It exists so that the round-trip check has something to fail on. `World`
/// folds each system's `hash` into `slots`, so a `DecaySystem` whose
/// `remainder_` came back as zero makes the very next turn hash differently --
/// which is what makes "did the save carry the systems?" an assertion rather
/// than a hope.
///
/// **Partition-invariant on purpose**: nothing here happens "per turn", every
/// effect is drained out of an accumulator over elapsed time. See
/// `sim/conformance.hpp` on why that is the discipline a real system follows.
class DecaySystem final : public System {
 public:
  static constexpr std::int32_t kInterval = 250;

  [[nodiscard]] std::string_view name() const noexcept override { return "decay"; }

  void advance(World& world, const Turn& turn) override {
    remainder_ += turn.length;
    while (remainder_ >= kInterval) {
      remainder_ -= kInterval;
      ++fires_;
      const std::span<const WorldObject> objects = world.objects();
      if (objects.empty()) continue;
      // Walk the table in id order, which is spawn order: iteration order is
      // state and this is what a real system does.
      const std::size_t index = static_cast<std::size_t>(cursor_ % objects.size());
      const ObjectId id = objects[index].id;
      cursor_ = (cursor_ + 1) % static_cast<std::uint32_t>(objects.size());
      const WorldObject* slot = world.find(id);
      if (slot == nullptr) continue;
      // One draw from the world's generator, per unit of elapsed time rather
      // than per turn: the RNG's own position in the stream is state.
      const std::int32_t bite = world.rng().below(3) + 1;
      world.set_health(id, slot->state.health - bite);
      world.set_stamina(id, slot->state.stamina + 1);
    }
  }

  void hash(std::uint64_t& accumulator) const noexcept override {
    accumulator ^= static_cast<std::uint64_t>(remainder_) * 1099511628211ULL;
    accumulator ^= static_cast<std::uint64_t>(fires_) * 1099511628211ULL;
    accumulator ^= static_cast<std::uint64_t>(cursor_) * 1099511628211ULL;
  }

  void serialize(std::vector<std::byte>& out) const {
    bytes::put_i32(out, remainder_);
    bytes::put_u64(out, fires_);
    bytes::put_u32(out, cursor_);
  }

  bool deserialize(std::span<const std::byte> data) {
    ByteReader reader(data);
    if (!bytes::get_i32(reader, remainder_) || !bytes::get_u64(reader, fires_) ||
        !reader.u32(cursor_)) {
      return false;
    }
    return reader.remaining() == 0;
  }

 private:
  std::int32_t remainder_ = 0;
  std::uint64_t fires_ = 0;
  std::uint32_t cursor_ = 0;
};

// --------------------------------------------------------------------------
// a world with something in every corner of the state vector
// --------------------------------------------------------------------------

/// Populate `world` so that every branch of `World::serialize` is exercised:
/// plain objects, a settlement composite (three consecutive internal handles),
/// a ship and its holder, the three singletons, a garrisoned object, a query
/// object, both name tables, a despawn that leaves a gap in the id sequence,
/// and a non-default player table.
void populate(World& world, std::uint32_t seed) {
  world.seed(seed);
  world.set_command_id_seed(0x168);

  const World::SettlementIds town = world.spawn_settlement(2);
  const ObjectId hall = world.spawn(NativeClass::town_hall, nullptr);
  const ObjectId barrack = world.spawn(NativeClass::barrack, nullptr);
  const World::ShipIds ship = world.spawn_ship(nullptr);

  std::vector<ObjectId> units;
  for (int i = 0; i < 6; ++i) {
    const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
    units.push_back(unit);
    world.set_position(unit, Point{100 + i * 37, 200 - i * 11});
    world.set_owner(unit, static_cast<PlayerId>(i % 3));
    world.set_health(unit, 40 + i * 3);
    world.set_stamina(unit, i);
    WorldObject* slot = world.find(unit);
    slot->settlement = town.settlement;
    slot->sight = 300 + i;
    slot->state.flags.hidden = (i % 2) == 1;
    slot->state.flags.has_active_path = (i % 3) == 0;
    // Every bit the packer can write has to be set on somebody here and clear
    // on somebody else, and comparing it below is only half of that.
    //
    // **This fixture stopped doing it and nothing noticed for eight bits.** It
    // used to read `in_party = (i % 4) == 2`, `unspawned = (i % 5) == 1`, and
    // on up to `messenger = (i % 15) == 12` -- one more modulus per bit, while
    // the loop stayed at six objects. From `building = (i % 9) == 6` onwards
    // the condition is *never true for any i*, so `building`, `autocast`,
    // `on_minimap`, `enemies_near`, `friends_near`, `gate_open` and
    // `messenger` were never set in any saved world in the suite -- exactly
    // the blind spot the comment above them was written to close, reopened by
    // the pattern the comment itself established. A fault injected into any of
    // their packing round-tripped clean.
    //
    // `(i + k) % 3 == 0` instead: every bit is set on two of the six and clear
    // on four, whatever the object count grows to, and the sweep below asserts
    // that rather than trusting it.
    const auto phase = [i](int k) { return (i + k) % 3 == 0; };
    slot->state.flags.in_party = phase(0);
    slot->state.flags.unspawned = phase(1);
    slot->state.flags.no_ai = phase(2);
    slot->state.flags.in_air = phase(3);
    slot->state.flags.noselect = phase(4);
    slot->state.flags.building = phase(5);
    slot->state.flags.autocast = phase(6);
    slot->state.flags.on_minimap = phase(7);
    slot->state.flags.enemies_near = phase(8);
    slot->state.flags.friends_near = phase(9);
    slot->state.flags.gate_open = phase(10);
    slot->state.flags.messenger = phase(11);
    slot->state.flags.landing = phase(12);
    slot->state.flags.built = phase(13);
    slot->state.flags.cursed = phase(14);
    slot->state.flags.ondie_fired = phase(15);
    slot->state.flags.diseased = phase(16);
    slot->state.flags.commands_disabled = phase(17);
    slot->state.flags.half_damage = phase(18);
    slot->state.flags.training = phase(19);
    // The flight altitudes, for the same reason: two ints that only
    // `Flying::PlayAnim` writes, and a build that dropped either would
    // round-trip clean over a fixture that left them at zero.
    slot->state.z_from = 40 + i;
    slot->state.z_to = 900 - i;
    slot->state.damage_taken = 7 * i;
    // The two effect tags, for the same reason: handles nothing here resolves,
    // which is what a build that dropped either would have to get wrong.
    slot->state.mist = 500 + static_cast<ObjectId>(i);
    slot->state.sheltered_by = 700 + static_cast<ObjectId>(i);
  }
  const World::SingletonIds singletons = world.spawn_singletons();
  (void)singletons;

  world.set_position(hall, Point{512, 512});
  world.set_health(hall, 900);
  world.set_owner(hall, 2);
  world.set_position(barrack, Point{600, 480});
  world.set_owner(barrack, 2);

  // A garrisoned object: its position is its holder's and its own reads
  // `kHeldPosition`. A save that dropped `holder` would put it back on the map.
  world.put_in_holder(units[0], ship.holder);

  // A gap in the id sequence. Handles are never reused, so `next_id_` has to
  // survive the round trip on its own.
  world.despawn(units[5]);

  // Groups, including one a map file would never have declared -- the shipped
  // scripts make those, so a save has to carry them.
  const std::int32_t attackers = world.group_index("Attackers");
  const std::int32_t mules = world.group_index("GoldMules2");
  // **Added out of ascending order, deliberately.** A group holds its members
  // in the order they joined, so a save has to carry that order rather than a
  // sorted one -- and this fixture used to add 1 then 2, which is sorted
  // either way. That is why the reader kept an `id <= last` check long after
  // the table stopped producing ascending ids: every save of every real map
  // was refused, and all of these tests passed, because none of their groups
  // could tell the difference.
  world.groups().add(attackers, units[2]);
  world.groups().add(attackers, units[1]);
  world.groups().add(mules, units[3]);
  // An empty group: interned by a `Group("...")` read that created it and
  // never added to. It is still an index every later group's numbering
  // depends on.
  world.group_index("Oasis_Guards");

  world.named_objects().bind("Caesar", hall);
  world.named_objects().bind("NO_Invisible", units[2]);
  // A binding whose object is gone: `NamedObj::IsDead` exists, so this is a
  // legal state and the save must reproduce it rather than tidy it away.
  world.named_objects().bind("Fallen", units[5]);

  ClassFilter filter;
  filter.match_all = false;
  filter.count = 2;
  filter.classes[0] = 3;
  filter.classes[1] = 7;
  world.create_query(objs_in_circle(Point{400, 400}, 250, filter));
  world.create_query(group_query(attackers));
  // A rectangle too. The four corners are the newest thing in a `QuerySpec` and
  // are the only fields no other query kind writes, so a save that dropped them
  // would round-trip every other query in this fixture perfectly.
  world.create_query(objs_in_rect(150, 150, 650, 650, filter));

  world.players().setup(0).name = "Caesar";
  world.players().setup(0).race = "Roman";
  world.players().setup(0).control = PlayerControl::human;
  world.players().setup(0).start = Point{700, 300};
  world.players().setup(1).name = "Vercingetorix";
  world.players().setup(1).control = PlayerControl::computer;
  world.players().setup(1).difficulty = 2;
  world.players().setup(1).colour = 0x1F3Fu;
  world.players().setup(1).ai_script = "DATA/AI/MAIN.VS";
  world.players().set(0, 1, Relation::allied, true);
  world.players().set_relation_word(2, 0, kRelationCeasefireOnly);
}

/// A world, a system, and the two of them wired together. Owned so a test can
/// build two independent ones and compare.
struct Game {
  World world;
  DecaySystem decay;

  Game() { world.add_system(&decay); }
  Game(const Game&) = delete;
  Game& operator=(const Game&) = delete;
};

[[nodiscard]] std::unique_ptr<Game> build(std::uint32_t seed) {
  auto game = std::make_unique<Game>();
  populate(game->world, seed);
  game->world.start();
  return game;
}

/// Save a `Game` whole: the world, plus the decay system's own section.
[[nodiscard]] std::vector<std::byte> save_game(const Game& game, std::string_view map = "") {
  std::vector<std::byte> decay;
  game.decay.serialize(decay);
  const SaveSection sections[] = {SaveSection{"decay", decay}};

  SaveInputs inputs;
  inputs.systems = sections;
  inputs.map = map;

  std::vector<std::byte> out;
  const Result<SaveReport> report = write_save(game.world, inputs, out);
  if (!report.ok()) return {};
  return out;
}

/// How much of a save a restore is allowed to skip. The `skip_*` values are the
/// deliberate faults: each one is a field a careless save could have left out,
/// and every one of them must be caught by the checks below.
enum class Fault {
  none,
  skip_system,  ///< restore the world but not the system's state
  skip_rng,     ///< restore everything, then put the RNG back to its seed
  skip_clock,   ///< restore everything, then wind the clock back
};

[[nodiscard]] bool load_game(Game& game, std::span<const std::byte> data, Fault fault) {
  const Result<SaveReader> reader = SaveReader::open(data);
  if (!reader.ok()) return false;

  LoadOptions options;
  const Result<LoadReport> report = read_save(reader.value(), game.world, options);
  if (!report.ok()) return false;

  if (fault != Fault::skip_system) {
    if (!game.decay.deserialize(reader->system_section("decay"))) return false;
  }
  if (fault == Fault::skip_rng) game.world.seed(1);
  if (fault == Fault::skip_clock) game.world.clock().advance(400);
  return true;
}

// --------------------------------------------------------------------------
// the world's own round trip
// --------------------------------------------------------------------------

TEST(save_world_round_trips_byte_identically) {
  const std::unique_ptr<Game> original = build(0x2545F491u);
  original->world.advance_turns(7);

  std::vector<std::byte> first;
  original->world.serialize(first);
  CHECK(!first.empty());

  Game restored;
  REQUIRE(restored.world.deserialize(first).ok());

  std::vector<std::byte> second;
  restored.world.serialize(second);
  // Byte-identical, not merely equivalent. A save that re-encodes differently
  // is a save whose meaning depends on which end of the round trip you are.
  CHECK(first == second);

  // `state_hash` folds every registered system in after the object table, so
  // the two worlds only agree once the system's own state is back too. That is
  // not a caveat -- it is the reason the system section exists.
  std::vector<std::byte> decay;
  original->decay.serialize(decay);
  REQUIRE(restored.decay.deserialize(decay));
  CHECK(restored.world.state_hash() == original->world.state_hash());
}

/// **Every writable flag bit is exercised in both directions by the fixture.**
///
/// This is the check the comment in `build` had been standing in for, and the
/// reason it now exists is that the comment was wrong for eight of the
/// eighteen bits: the moduli grew one per bit while the object loop stayed at
/// six, so from `building` onwards the condition was never true for any object
/// and nothing in the suite had ever saved one of those bits set.
///
/// Asserted over the fixture rather than over the round trip, because a bit
/// that is never set round-trips perfectly and tells you nothing.
TEST(save_the_fixture_sets_and_clears_every_writable_flag) {
  const std::unique_ptr<Game> game = build(0x2468ACEu);

  using Bit = bool ObjectFlags::*;
  struct Named {
    const char* name;
    Bit bit;
  };
  // `is_unit`, `is_building` and `is_hero` are the native class's and are not
  // free to set, so they are covered by having units, buildings and a hero in
  // the fixture rather than by this sweep.
  static constexpr Named kWritable[] = {
      {"hidden", &ObjectFlags::hidden},
      {"has_active_path", &ObjectFlags::has_active_path},
      {"in_party", &ObjectFlags::in_party},
      {"unspawned", &ObjectFlags::unspawned},
      {"no_ai", &ObjectFlags::no_ai},
      {"built", &ObjectFlags::built},
      {"in_air", &ObjectFlags::in_air},
      {"noselect", &ObjectFlags::noselect},
      {"building", &ObjectFlags::building},
      {"autocast", &ObjectFlags::autocast},
      {"on_minimap", &ObjectFlags::on_minimap},
      {"enemies_near", &ObjectFlags::enemies_near},
      {"friends_near", &ObjectFlags::friends_near},
      {"gate_open", &ObjectFlags::gate_open},
      {"messenger", &ObjectFlags::messenger},
      {"landing", &ObjectFlags::landing},
      {"cursed", &ObjectFlags::cursed},
      {"ondie_fired", &ObjectFlags::ondie_fired},
      {"diseased", &ObjectFlags::diseased},
  };

  for (const Named& named : kWritable) {
    bool any_set = false;
    bool any_clear = false;
    for (const WorldObject& slot : game->world.objects()) {
      if (slot.state.flags.*named.bit) {
        any_set = true;
      } else {
        any_clear = true;
      }
    }
    if (!any_set || !any_clear) {
      std::printf("  flag %s: set on %d object(s), clear on %d\n", named.name,
                  any_set ? 1 : 0, any_clear ? 1 : 0);
    }
    CHECK(any_set);
    CHECK(any_clear);
  }
}

TEST(save_world_restores_every_field_it_claims) {
  const std::unique_ptr<Game> original = build(0x13579BDFu);
  original->world.advance_turns(3);

  std::vector<std::byte> data;
  original->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  const World& a = original->world;
  const World& b = restored.world;

  CHECK(a.turns() == b.turns());
  CHECK(a.time() == b.time());
  CHECK(a.clock().turn_length() == b.clock().turn_length());
  CHECK(a.clock().config().game_speed == b.clock().config().game_speed);
  CHECK(a.clock().turn().index == b.clock().turn().index);
  CHECK(a.clock().turn().start == b.clock().turn().start);
  CHECK(a.clock().turn().end == b.clock().turn().end);
  CHECK(a.rng().state() == b.rng().state());
  CHECK(a.next_id() == b.next_id());
  CHECK(a.size() == b.size());

  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    const WorldObject& x = a.objects()[i];
    const WorldObject& y = b.objects()[i];
    CHECK(x.id == y.id);
    CHECK(x.internal == y.internal);
    CHECK(x.class_index == y.class_index);
    CHECK(x.settlement == y.settlement);
    CHECK(x.sight == y.sight);
    CHECK(x.query == y.query);
    CHECK(x.state.position == y.state.position);
    CHECK(x.state.owner == y.state.owner);
    CHECK(x.state.holder == y.state.holder);
    CHECK(x.state.health == y.state.health);
    CHECK(x.state.stamina == y.state.stamina);
    CHECK(x.state.flags.hidden == y.state.flags.hidden);
    CHECK(x.state.flags.has_active_path == y.state.flags.has_active_path);
    CHECK(x.state.flags.is_unit == y.state.flags.is_unit);
    CHECK(x.state.flags.is_building == y.state.flags.is_building);
    CHECK(x.state.flags.is_hero == y.state.flags.is_hero);
    CHECK(x.state.flags.in_party == y.state.flags.in_party);
    CHECK(x.state.flags.unspawned == y.state.flags.unspawned);
    CHECK(x.state.flags.no_ai == y.state.flags.no_ai);
    CHECK(x.state.flags.in_air == y.state.flags.in_air);
    CHECK(x.state.flags.noselect == y.state.flags.noselect);
    CHECK(x.state.flags.building == y.state.flags.building);
    CHECK(x.state.flags.autocast == y.state.flags.autocast);
    CHECK(x.state.flags.on_minimap == y.state.flags.on_minimap);
    CHECK(x.state.flags.enemies_near == y.state.flags.enemies_near);
    CHECK(x.state.flags.friends_near == y.state.flags.friends_near);
    CHECK(x.state.flags.gate_open == y.state.flags.gate_open);
    CHECK(x.state.flags.messenger == y.state.flags.messenger);
    CHECK(x.state.flags.landing == y.state.flags.landing);
    CHECK(x.state.flags.cursed == y.state.flags.cursed);
    CHECK(x.state.flags.diseased == y.state.flags.diseased);
    CHECK(x.state.flags.ondie_fired == y.state.flags.ondie_fired);
    CHECK(x.state.flags.commands_disabled == y.state.flags.commands_disabled);
    CHECK(x.state.flags.half_damage == y.state.flags.half_damage);
    CHECK(x.state.flags.training == y.state.flags.training);
    CHECK(x.state.z_from == y.state.z_from);
    CHECK(x.state.damage_taken == y.state.damage_taken);
    CHECK(x.state.z_to == y.state.z_to);
    CHECK(x.state.mist == y.state.mist);
    CHECK(x.state.sheltered_by == y.state.sheltered_by);
    CHECK((x.object == nullptr) == (y.object == nullptr));
    if (x.object != nullptr) {
      CHECK(x.object->native_class() == y.object->native_class());
      CHECK(x.object->anim.state_idx == y.object->anim.state_idx);
      CHECK(x.object->anim.anim_slot == y.object->anim.anim_slot);
      CHECK(x.object->anim.elapsed_ms == y.object->anim.elapsed_ms);
    }
  }

  // The two name tables, index by index: the indices are what a live
  // `Group(...)` handle and a stored `QuerySpec::group` refer to.
  REQUIRE(a.groups().size() == b.groups().size());
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(a.groups().size()); ++i) {
    CHECK(a.groups().name(i) == b.groups().name(i));
    const std::span<const ObjectId> left = a.groups().members(i);
    const std::span<const ObjectId> right = b.groups().members(i);
    REQUIRE(left.size() == right.size());
    for (std::size_t m = 0; m < left.size(); ++m) CHECK(left[m] == right[m]);
  }
  REQUIRE(a.named_objects().size() == b.named_objects().size());
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(a.named_objects().size()); ++i) {
    CHECK(a.named_objects().name(i) == b.named_objects().name(i));
    CHECK(a.named_objects().object(i) == b.named_objects().object(i));
  }
  // A binding to a despawned object survives, because `NamedObj::IsDead` is a
  // question a script is allowed to ask.
  CHECK(b.named_object("Fallen") != kNoObject);
  CHECK(b.named_object_alive("Fallen") == kNoObject);

  CHECK(a.players().setup(0).name == b.players().setup(0).name);
  CHECK(a.players().setup(1).ai_script == b.players().setup(1).ai_script);
  CHECK(a.players().setup(1).colour == b.players().setup(1).colour);
  CHECK(b.players().has(0, 1, Relation::allied));
  CHECK(a.players().relation_word(2, 0) == b.players().relation_word(2, 0));
}

TEST(save_world_reproduces_a_query_it_restored) {
  const std::unique_ptr<Game> original = build(7);
  std::vector<ObjectId> before;
  const ObjectId query = original->world.objects().back().id;
  (void)query;

  std::vector<std::byte> data;
  original->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  // Find the group query in both and evaluate it. A query object whose *spec*
  // came back wrong would still be a live handle answering a different
  // question, which no field-by-field comparison of the object header sees.
  std::size_t compared = 0;
  for (const WorldObject& slot : original->world.objects()) {
    if (slot.internal != InternalKind::query) continue;
    std::vector<ObjectId> a;
    std::vector<ObjectId> b;
    original->world.evaluate_query(slot.id, a);
    restored.world.evaluate_query(slot.id, b);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
    ++compared;
  }
  CHECK(compared == 3);
}

TEST(save_world_carries_a_query_rectangle) {
  // `save_world_reproduces_a_query_it_restored` above compares *answers*, and a
  // rectangle can answer identically to a circle over one object layout, so
  // this compares the spec itself, corner by corner. Both are needed: the spec
  // check would miss a restored spec that no longer evaluates, and the answer
  // check would miss a corner restored into the wrong field.
  const std::unique_ptr<Game> original = build(3);
  std::vector<std::byte> data;
  original->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  std::size_t rectangles = 0;
  for (const WorldObject& slot : original->world.objects()) {
    if (slot.internal != InternalKind::query) continue;
    const QuerySpec* before = original->world.query_spec(slot.id);
    const QuerySpec* after = restored.world.query_spec(slot.id);
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);
    CHECK(*before == *after);
    if (before->kind != QueryKind::map_area_rect) continue;
    ++rectangles;
    CHECK(after->left == 150);
    CHECK(after->top == 150);
    CHECK(after->right == 650);
    CHECK(after->bottom == 650);
  }
  // The precondition: the fixture really did build one. Without this the loop
  // above passes on a world with no rectangle in it.
  CHECK(rectangles == 1);

  // And the whole world still hashes the same, which is the check that sees a
  // corner restored into the wrong field even if `operator==` were ever
  // loosened.
  CHECK(original->world.state_hash() == restored.world.state_hash());
}

TEST(save_world_refuses_a_malformed_table) {
  const std::unique_ptr<Game> game = build(11);
  std::vector<std::byte> data;
  game->world.serialize(data);

  Game target;
  // Truncation at every length: not one of them may load, and none may leave
  // the world half-written -- `deserialize` is atomic, so the target is still
  // empty afterwards.
  for (std::size_t cut = 0; cut < data.size(); cut += 17) {
    const Status status = target.world.deserialize(std::span(data).first(cut));
    CHECK(!status.ok());
  }
  CHECK(target.world.size() == 0);

  // A trailing byte the writer never wrote.
  std::vector<std::byte> extra = data;
  extra.push_back(std::byte{0});
  CHECK(target.world.deserialize(extra).error() == FormatError::malformed);

  // A wrong magic and a wrong version are different refusals.
  std::vector<std::byte> bad_magic = data;
  bad_magic[0] = std::byte{'X'};
  CHECK(target.world.deserialize(bad_magic).error() == FormatError::bad_magic);
  std::vector<std::byte> bad_version = data;
  bad_version[4] = std::byte{99};
  CHECK(target.world.deserialize(bad_version).error() == FormatError::unsupported);
  CHECK(target.world.size() == 0);
}

TEST(save_world_refuses_a_pipeline_it_does_not_have) {
  const std::unique_ptr<Game> game = build(13);
  std::vector<std::byte> data;
  game->world.serialize(data);

  // A world with no systems must not accept a save written by one that had
  // one: run order is folded into `slots` ahead of every system's own
  // contribution, so the hashes would be incomparable from turn one.
  World bare;
  CHECK(bare.deserialize(data).error() == FormatError::unsupported);
}

// --------------------------------------------------------------------------
// the envelope
// --------------------------------------------------------------------------

TEST(save_envelope_round_trips_its_sections) {
  const std::unique_ptr<Game> game = build(17);
  game->world.advance_turns(4);

  SelectionTable selections;
  selections.select(0, game->world.objects()[4].id);
  selections.select(3, game->world.objects()[5].id);

  std::vector<std::byte> decay;
  game->decay.serialize(decay);
  const SaveSection sections[] = {SaveSection{"decay", decay}};

  SaveInputs inputs;
  inputs.selections = &selections;
  inputs.systems = sections;
  inputs.map = "Maps/Numantia";

  std::vector<std::byte> data;
  const Result<SaveReport> written = write_save(game->world, inputs, data);
  REQUIRE(written.ok());
  CHECK(written->unsaved_systems.empty());
  CHECK(written->bytes == data.size());

  const Result<SaveReader> reader = SaveReader::open(data);
  REQUIRE(reader.ok());
  CHECK(reader->meta().map == "Maps/Numantia");
  CHECK(reader->meta().turns == game->world.turns());
  CHECK(reader->meta().time == game->world.time());
  CHECK(reader->meta().slots == game->world.state_hash());
  CHECK(reader->meta().state_vector == kStateVectorVersion);
  REQUIRE(reader->meta().systems.size() == 1);
  CHECK(reader->meta().systems[0] == "decay");
  CHECK(reader->has(kWorldSection));
  CHECK(reader->has(kSelectionSection));
  CHECK(!reader->system_section("decay").empty());
  // The meta block is first, so a reader can refuse on version or map before
  // decoding an object table.
  REQUIRE(reader->names().size() == 4);
  CHECK(reader->names()[0] == "meta");

  Game target;
  SelectionTable restored_selections;
  LoadOptions options;
  options.selections = &restored_selections;
  options.map = "Maps/Numantia";
  const Result<LoadReport> report = read_save(data, target.world, options);
  REQUIRE(report.ok());
  CHECK(report->objects == game->world.size());
  CHECK(report->unconsumed.empty());
  CHECK(report->unrestored_systems.empty());
  CHECK(restored_selections.player(0).ids().size() == 1);
  CHECK(restored_selections.player(3).ids().size() == 1);

  CHECK(target.decay.deserialize(reader->system_section("decay")));
  CHECK(verify_hashes(target.world, reader->meta()).ok);
}

TEST(save_envelope_refuses_what_it_should) {
  const std::unique_ptr<Game> game = build(19);
  const std::vector<std::byte> data = save_game(*game, "Maps/Numantia");
  REQUIRE(!data.empty());

  CHECK(SaveReader::open({}).error() == FormatError::truncated);

  std::vector<std::byte> bad_magic = data;
  bad_magic[1] = std::byte{'X'};
  CHECK(SaveReader::open(bad_magic).error() == FormatError::bad_magic);

  // The format version and the state-vector version are separate words and
  // separate reasons, and both are hard refusals. This is the check that keeps
  // a save written today from loading into a build whose state vector moved.
  std::vector<std::byte> bad_format = data;
  bad_format[4] = std::byte{kSaveFormatVersion + 1};
  CHECK(SaveReader::open(bad_format).error() == FormatError::unsupported);

  std::vector<std::byte> bad_vector = data;
  bad_vector[8] = std::byte{kStateVectorVersion + 1};
  CHECK(SaveReader::open(bad_vector).error() == FormatError::unsupported);

  // And the refusal says which: the version found and the version read, so a
  // save an older build wrote -- the owner's playtest saves at state vector 21
  // when 22 made script points two 32-bit words -- reads as the build's
  // refusal and not as a broken file.
  std::vector<std::byte> older = data;
  older[8] = std::byte{kStateVectorVersion - 1};
  CHECK(SaveReader::open(older).error() == FormatError::unsupported);
  CHECK(save_version_refusal(older) ==
        "written with state vector " + std::to_string(kStateVectorVersion - 1) +
            "; this build reads state vector " + std::to_string(kStateVectorVersion));
  CHECK(save_version_refusal(bad_format) ==
        "written with save format " + std::to_string(kSaveFormatVersion + 1) +
            "; this build reads save format " + std::to_string(kSaveFormatVersion));
  CHECK(save_version_refusal(data).empty());
  CHECK(save_version_refusal(bad_magic).empty());
  // Not a save at all says nothing about versions, even where its third word
  // happens not to be this build's state vector.
  std::vector<std::byte> stranger = older;
  stranger[1] = std::byte{'X'};
  CHECK(save_version_refusal(stranger).empty());
  CHECK(save_version_refusal({}).empty());
  CHECK(save_version_refusal(std::span<const std::byte>(data).first(10)).empty());

  std::vector<std::byte> truncated = data;
  truncated.resize(data.size() - 1);
  CHECK(!SaveReader::open(truncated).ok());

  std::vector<std::byte> trailing = data;
  trailing.push_back(std::byte{0});
  CHECK(SaveReader::open(trailing).error() == FormatError::malformed);

  // A map the save was not made on. The original refuses the same way and
  // prints "The map you played this game on when the game was saved (%s1) is
  // missing or has been changed".
  Game target;
  LoadOptions options;
  options.map = "Maps/Alesia";
  CHECK(read_save(data, target.world, options).error() == FormatError::not_found);
  CHECK(target.world.size() == 0);

  // A world whose pipeline is not the saved one.
  World bare;
  LoadOptions bare_options;
  CHECK(read_save(data, bare, bare_options).error() == FormatError::unsupported);
}

TEST(save_envelope_names_the_systems_it_could_not_save) {
  const std::unique_ptr<Game> game = build(23);

  // No section for `decay`: the save is written, and it says so.
  SaveInputs inputs;
  std::vector<std::byte> data;
  const Result<SaveReport> written = write_save(game->world, inputs, data);
  REQUIRE(written.ok());
  REQUIRE(written->unsaved_systems.size() == 1);
  CHECK(written->unsaved_systems[0] == "decay");

  Game target;
  LoadOptions options;
  const Result<LoadReport> report = read_save(data, target.world, options);
  REQUIRE(report.ok());
  REQUIRE(report->unrestored_systems.size() == 1);
  CHECK(report->unrestored_systems[0] == "decay");
}

TEST(save_writer_refuses_a_duplicate_or_malformed_section) {
  SaveWriter writer(SaveMeta{});
  const std::byte payload[] = {std::byte{1}};
  CHECK(writer.add("world", payload).ok());
  CHECK(writer.add("world", payload).error() == FormatError::malformed);
  CHECK(writer.add("", payload).error() == FormatError::malformed);
  // The writer owns the meta section; a caller must not be able to shadow it.
  CHECK(writer.add("meta", payload).error() == FormatError::malformed);
  CHECK(writer.add(std::string_view("bad\nname"), payload).error() == FormatError::malformed);
  CHECK(writer.sections().size() == 1);
  // Deterministic: the same writer produces the same bytes every time.
  CHECK(writer.finish() == writer.finish());
}

// --------------------------------------------------------------------------
// the hash check, and proof that it catches something
// --------------------------------------------------------------------------

TEST(save_verify_hashes_catches_an_unrestored_system) {
  const std::unique_ptr<Game> game = build(29);
  game->world.advance_turns(9);
  const std::vector<std::byte> data = save_game(*game);
  REQUIRE(!data.empty());

  const Result<SaveReader> reader = SaveReader::open(data);
  REQUIRE(reader.ok());

  // The honest restore passes.
  Game good;
  REQUIRE(load_game(good, data, Fault::none));
  CHECK(verify_hashes(good.world, reader->meta()).ok);

  // The one that skipped the system's own section does not, and names the
  // channel. This is the check earning its place: `slots` is where a system's
  // contribution lands, and the mismatch is visible at load rather than as a
  // divergence twenty turns later.
  Game broken;
  REQUIRE(load_game(broken, data, Fault::skip_system));
  const HashMismatch mismatch = verify_hashes(broken.world, reader->meta());
  CHECK(!mismatch.ok);
  CHECK(mismatch.channel == "slots");
  CHECK(mismatch.expected == reader->meta().slots);
  CHECK(mismatch.expected != mismatch.actual);
}

// --------------------------------------------------------------------------
// the property: a load reproduces the simulation
// --------------------------------------------------------------------------

/// A `Run` that owns its world and its system, so a scenario can hand back a
/// live game rather than a reference into something that has gone out of scope.
class GameRun final : public conformance::Run {
 public:
  explicit GameRun(std::unique_ptr<Game> game) noexcept : game_(std::move(game)) {}
  [[nodiscard]] World& world() noexcept override { return game_->world; }
  void advance(std::int32_t turn_length) override { game_->world.advance(turn_length); }

 private:
  std::unique_ptr<Game> game_;
};

/// Build the game and run `warmup` turns. The reference: no save involved.
class DirectScenario final : public conformance::Scenario {
 public:
  explicit DirectScenario(std::size_t warmup) noexcept : warmup_(warmup) {}
  [[nodiscard]] std::string_view name() const noexcept override { return "direct"; }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
    std::unique_ptr<Game> game = build(seed);
    for (std::size_t i = 0; i < warmup_; ++i) game->world.advance(400);
    return std::make_unique<GameRun>(std::move(game));
  }

 private:
  std::size_t warmup_;
};

/// The same, then saved and loaded into a **fresh** world before the harness
/// touches it. If the save carries the game, the two scenarios are
/// indistinguishable from here on.
class RestoredScenario final : public conformance::Scenario {
 public:
  RestoredScenario(std::size_t warmup, Fault fault) noexcept : warmup_(warmup), fault_(fault) {}
  [[nodiscard]] std::string_view name() const noexcept override { return "restored"; }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
    std::unique_ptr<Game> game = build(seed);
    for (std::size_t i = 0; i < warmup_; ++i) game->world.advance(400);

    const std::vector<std::byte> data = save_game(*game);
    if (data.empty()) return nullptr;
    auto fresh = std::make_unique<Game>();
    if (!load_game(*fresh, data, fault_)) return nullptr;
    return std::make_unique<GameRun>(std::move(fresh));
  }

 private:
  std::size_t warmup_;
  Fault fault_;
};

TEST(save_round_trip_reproduces_the_simulation) {
  constexpr std::uint32_t kSeed = 0x5EEDC0DEu;
  constexpr std::size_t kWarmup = 12;
  // Not a uniform schedule: the turn length is renegotiated in a real session
  // and a bug that only shows when it changes is the one worth catching.
  const std::vector<std::int32_t> schedule{400, 400, 800, 200, 200, 799,
                                           400, 800, 200, 400, 400, 800};

  const DirectScenario direct(kWarmup);
  const RestoredScenario restored(kWarmup, Fault::none);

  const conformance::Trace expected = conformance::record(direct, kSeed, schedule);
  const conformance::Trace actual = conformance::record(restored, kSeed, schedule);
  REQUIRE(expected.size() == schedule.size());
  REQUIRE(actual.size() == schedule.size());

  // The turn numbering continues from the save, so the two traces are directly
  // comparable: both start at turn `kWarmup + 1`.
  CHECK(expected.entries.front().turn == kWarmup + 1);
  CHECK(actual.entries.front().turn == kWarmup + 1);

  const conformance::Divergence divergence = conformance::compare_traces(expected, actual);
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(!divergence.diverged());

  // And the reserved channels stayed reserved: a save must not be the thing
  // that pulls the pathfinder or the fog into hashed state.
  CHECK(!conformance::check_reserved_channels(actual).diverged());

  // The save/load path is itself deterministic: two loads of the same save
  // produce the same run, which is what makes a save shareable between peers.
  const conformance::Divergence self =
      conformance::check_self_consistency(restored, kSeed, schedule);
  if (self.diverged()) std::printf("  %s\n", self.describe().c_str());
  CHECK(!self.diverged());
}

TEST(save_round_trip_check_catches_a_dropped_field) {
  // A check that has never been seen to fail is not a check. Each fault below
  // is a field a save could plausibly have left out; the harness must catch
  // every one and name the channel.
  constexpr std::uint32_t kSeed = 0x5EEDC0DEu;
  constexpr std::size_t kWarmup = 12;
  const std::vector<std::int32_t> schedule = conformance::uniform_schedule(400, 8);

  const DirectScenario direct(kWarmup);
  const conformance::Trace expected = conformance::record(direct, kSeed, schedule);
  REQUIRE(expected.size() == schedule.size());

  const Fault faults[] = {Fault::skip_system, Fault::skip_rng, Fault::skip_clock};
  for (const Fault fault : faults) {
    const RestoredScenario broken(kWarmup, fault);
    const conformance::Trace actual = conformance::record(broken, kSeed, schedule);
    REQUIRE(actual.size() == schedule.size());
    const conformance::Divergence divergence = conformance::compare_traces(expected, actual);
    CHECK(divergence.diverged());
    // Every one of them lands in `slots`, which is the channel the object
    // table and the systems fold into -- and the first turn is turn one,
    // because a dropped field is wrong immediately rather than eventually.
    CHECK(divergence.channel == conformance::Channel::slots ||
          divergence.kind == conformance::Divergence::Kind::time);
  }
}


// --------------------------------------------------------------------------
// the ten systems, and the pool
// --------------------------------------------------------------------------
//
// `core_tests` links `imperivm_core` alone -- "if a test needs a file or a
// window, it is testing the wrong layer" -- so the real-map round trip lives in
// `engine/tools/imsave.cpp`. What belongs here is the part that needs no map:
// each system's own section, mutated, encoded, decoded into a *fresh* system,
// and encoded again. A field nobody wrote round-trips perfectly as a zero, so
// the assertion is not "it decoded" but **"the re-encode is byte-identical to
// the encode, and the fresh system is not the default one"**.

/// Encode `system`, decode into `fresh`, encode again, and require the two
/// blobs to match. Returns the blob so a caller can assert on its size.
template <typename System>
[[nodiscard]] std::vector<std::byte> section_round_trips(const System& system, System& fresh) {
  std::vector<std::byte> first;
  system.serialize(first);
  if (!fresh.deserialize(first).ok()) return {};
  std::vector<std::byte> second;
  fresh.serialize(second);
  return first == second ? first : std::vector<std::byte>{};
}

/// A fresh system of the same type, encoded. The baseline every section below
/// has to differ from: a "round trip" that happens to reproduce the default
/// state proves nothing.
template <typename System>
[[nodiscard]] std::vector<std::byte> default_section() {
  System fresh;
  std::vector<std::byte> out;
  fresh.serialize(out);
  return out;
}

TEST(save_movement_section_round_trips) {
  World world;
  MovementSystem movement;
  world.add_system(&movement);
  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);

  MoveState& first = movement.state(a);
  first.speed = 220;
  first.speed_factor = 150;
  first.formation = "Wedge";
  first.target = Point{900, 400};
  first.target_object = b;
  first.has_path = true;
  first.goto_active = true;
  // The walk-cycle memory: left out of the section once, and a loaded session
  // replayed every moving unit's walk from step zero on its first turn.
  first.walking = true;
  first.progress = 123456789;
  // `Goto`'s failure stamp: the give-up and the re-search draw read it.
  first.goto_failed_at = 31337;
  first.waypoints = {Point{10, 20}, Point{30, 40}, Point{900, 400}};
  first.path_length = 4242;
  first.path_generation = 7;
  first.path_complete = true;
  first.last_outcome = MoveOutcome::moving;
  movement.state(b).facing = Point{-1, 0};

  MovementSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(movement, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<MovementSystem>());

  const MoveState* restored = fresh.find(a);
  REQUIRE(restored != nullptr);
  CHECK(restored->speed == 220);
  CHECK(restored->formation == "Wedge");
  CHECK(restored->target_object == b);
  CHECK(restored->progress == 123456789);
  CHECK(restored->walking);
  CHECK(!fresh.find(b)->walking);
  CHECK(restored->goto_failed_at == 31337);
  CHECK(fresh.find(b)->goto_failed_at == kNoGotoFailure);
  // The route is not hashed -- `pathfinder` is zero in all nine dumps -- and is
  // written anyway, because a route is not recomputable from the state around
  // it. A unit that came back with an empty path would silently stop.
  REQUIRE(restored->waypoints.size() == 3);
  CHECK((restored->waypoints[2] == Point{900, 400}));
  CHECK(restored->path_generation == 7);
  CHECK(fresh.tracked() == 2);
}

TEST(save_combat_section_round_trips) {
  CombatSystem combat;
  Combatant first;
  first.id = 11;
  first.owner = 1;
  first.position = Point{300, 400};
  first.health = 57;
  first.experience = 900;
  first.level = 4;
  first.target = 12;
  first.action = Action::engaging;
  first.next_action_time = 5500;
  first.damage_multiplier_percent = 200;
  first.ignores_armour = true;
  // `Unit::AddBonus`'s record, with all five distinct and two of them negative:
  // a section that dropped a field, or read the five back in the wrong order,
  // survives every zero and every repeated number.
  first.bonus.attack = 5;
  first.bonus.armour_slash = -10;
  first.bonus.armour_pierce = 7;
  first.bonus.max_health = -40;
  first.bonus.max_stamina = 90;
  combat.add(first);

  Combatant second = first;
  second.id = 12;
  second.owner = 2;
  second.target = 11;
  second.action = Action::dying;
  second.alive = false;
  combat.add(second);

  combat.set_player_level_addend(1, 3);
  combat.set_allied(1, 3, true);
  combat.set_death_duration(1750);

  CombatSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(combat, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<CombatSystem>());

  const Combatant* restored = fresh.find(11);
  REQUIRE(restored != nullptr);
  CHECK(restored->health == 57);
  CHECK(restored->action == Action::engaging);
  CHECK(restored->damage_multiplier_percent == 200);
  CHECK(restored->ignores_armour);
  CHECK(restored->bonus.attack == 5);
  CHECK(restored->bonus.armour_slash == -10);
  CHECK(restored->bonus.armour_pierce == 7);
  CHECK(restored->bonus.max_health == -40);
  CHECK(restored->bonus.max_stamina == 90);
  CHECK(fresh.player_level_addend(1) == 3);
  CHECK(fresh.combatants().size() == 2);
  // `Action` is not contiguous -- 0, 2, 3, 8 -- so a byte in one of its gaps is
  // a state no switch has an arm for and is refused rather than read as `idle`.
  std::vector<std::byte> corrupt;
  combat.serialize(corrupt);
  bool poked = false;
  for (std::size_t i = 0; i + 1 < corrupt.size(); ++i) {
    if (corrupt[i] == std::byte{2} && !poked) {
      corrupt[i] = std::byte{5};
      poked = true;
    }
  }
  CombatSystem target;
  CHECK(!target.deserialize(corrupt).ok());
}

TEST(save_economy_section_round_trips) {
  EconomySystem economy;
  SettlementInit init;
  init.settlement_object = 10;
  init.holder_object = 11;
  init.warehouse_object = 12;
  init.anchor = 13;
  init.owner = 2;
  init.kind = SettlementKind::stronghold;
  init.produces_gold = true;
  init.efficiency = 100;
  init.gold = 400;
  init.food = 250;
  init.max_gold = 5000;
  init.max_food = 3000;
  init.population = 12;
  init.max_population = 40;
  const SettlementId town = economy.create(init);
  REQUIRE(town != kNoSettlement);
  init.settlement_object = 20;
  init.holder_object = 21;
  init.warehouse_object = 22;
  init.kind = SettlementKind::village;
  const SettlementId village = economy.create(init);
  // Garrisoning first: `garrison_force_add` raises loyalty by
  // `loyalty_increase_per_unit`, so setting it afterwards is what pins the
  // value this test asserts on.
  (void)economy.garrison_force_add(town, 99);
  (void)economy.set_loyalty(town, 77);
  (void)economy.set_supplied(village, town);
  const std::uint32_t mule = economy.create_mule(village, town, Resource::food, 60);
  CHECK(mule != 0);
  // And a wagon sent after a unit rather than to a settlement.
  const std::uint32_t larder = economy.create_feeding_mule(town, 77, 30);
  CHECK(larder != 0);

  EconomySystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(economy, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<EconomySystem>());

  const Settlement* restored = fresh.find(town);
  REQUIRE(restored != nullptr);
  CHECK(restored->loyalty == 77);
  CHECK(restored->warehouse.gold == 400);
  CHECK(restored->holder.units.size() == 1);
  REQUIRE(fresh.wagons().size() == 2);
  CHECK(fresh.wagons()[0].follow == kNoObject);
  CHECK(fresh.wagons()[1].follow == 77);
  CHECK(fresh.wagons()[1].destination == kNoSettlement);
  CHECK(fresh.settlements().size() == 2);
  // The ten interval timers are what makes a turn splittable, and the bug that
  // found them was every settlement on every retail map starting at zero.
  CHECK(restored->timers == economy.find(town)->timers);
}

TEST(save_feeder_section_round_trips) {
  FeederSystem feeder;
  FeedingUnit link;
  link.unit = 5;
  link.food = 7;
  link.max_food = 10;
  link.max_health = 120;
  link.search_due = 3000;
  feeder.enrol(link);
  link.unit = 9;
  link.feeds = false;
  feeder.enrol(link);
  CHECK(feeder.set_food(9, 0));

  FeederSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(feeder, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<FeederSystem>());
  CHECK(fresh.total_unit_count() == 2);
  CHECK(fresh.food(5) == 7);
  CHECK(!fresh.feeding(9));
}

TEST(save_hero_section_round_trips) {
  World world;
  HeroSystem hero;
  world.add_system(&hero);
  const ObjectId leader = world.spawn(NativeClass::unit, nullptr);
  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr);
  world.set_owner(leader, 1);
  world.set_owner(soldier, 1);

  hero.register_hero(world, leader);
  hero.register_unit(world, soldier);
  CHECK(hero.attach(world, soldier, leader));
  // Points first: `set_skill` refuses to spend more than is available rather
  // than clamping, because the shipped AI computes the sum itself. The
  // balance derives from the level, one a level, over the offered skills.
  for (bool& offered : hero.hero(leader)->offered) offered = true;
  CHECK(hero.set_level(leader, 8));
  CHECK(hero.set_skill(leader, HeroSkill::administration, 3));
  CHECK(hero.set_experience(soldier, 640));
  // When the army was last struck, and by which member. Two more fields in the
  // hero record, and the ones `Hero::TimePastLastAttack` reads.
  record_army_attacked(world, soldier, 4321);

  HeroSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(hero, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<HeroSystem>());
  CHECK(fresh.skill(leader, HeroSkill::administration) == 3);
  CHECK(fresh.available_skill_points(leader) == 5);
  CHECK(fresh.experience(soldier) == 640);
  CHECK(fresh.hero_of(soldier) == leader);
  CHECK(fresh.army_size(leader) == 1);
  // The squad table came with it: a hero's army is a squad, and `squad_of` is
  // what `AIGetSquad` answers with.
  CHECK(fresh.squad_of(soldier) == hero.squad_of(soldier));
  REQUIRE(fresh.hero(leader) != nullptr);
  CHECK(fresh.hero(leader)->army_attacked_at == 4321);
  CHECK(fresh.hero(leader)->army_attacked_unit == soldier);
  // Both are hashed, so a dropped field is a hash mismatch and not only a
  // missing value -- which is the check that survives a future reader that
  // stops looking at the fields by name.
  std::uint64_t before = 0;
  std::uint64_t after = 0;
  hero.hash(before);
  fresh.hash(after);
  CHECK(before == after);
  hero.hero(leader)->army_attacked_at = 9999;
  std::uint64_t moved = 0;
  hero.hash(moved);
  CHECK(moved != before);

  // **And the section version has to move when a field joins one.** Every
  // system section carries `kSectionVersion` in its own bytes, four bytes in
  // after the magic; a reader that accepted the previous number would decode a
  // shorter record as a longer one and hand back rubbish rather than an error.
  // Without this the only thing pinning a bump is a comment, and a fault sweep
  // that reverts one survives.
  REQUIRE(bytes.size() > 8);
  std::vector<std::byte> older = bytes;
  const auto previous = static_cast<std::uint8_t>(older[4]) - 1;
  older[4] = static_cast<std::byte>(previous);
  HeroSystem refused;
  CHECK(!refused.deserialize(older).ok());
}

TEST(save_env_section_round_trips) {
  EnvSystem env;
  env.env().write_int(EnvScope::for_player(2), "Aggression", 45);
  env.env().write_string(EnvScope::root(), "Phase", "assault");
  env.env().write_object(EnvScope::for_settlement(3), "Target", 4242);
  CHECK(env.ai_vars().set(1, 12, 99));
  CHECK(env.ai_vars().set(1, 40, -1));
  CHECK(env.ai_vars().set(4, 0, 7));

  EnvSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(env, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<EnvSystem>());
  CHECK(fresh.env().read_int(EnvScope::for_player(2), "Aggression") == 45);
  CHECK(fresh.env().read_string(EnvScope::root(), "Phase") == "assault");
  CHECK(fresh.env().read_object(EnvScope::for_settlement(3), "Target") == 4242);
  CHECK(fresh.ai_vars().get(1, 12) == 99);
  CHECK(fresh.ai_vars().get(4, 0) == 7);
  // Both stores fold into the world hash, so equality of the hash is the
  // property that matters and it is checked directly.
  std::uint64_t before = 0;
  std::uint64_t after = 0;
  env.hash(before);
  fresh.hash(after);
  CHECK(before == after);
}

/// The three checks the loader makes on the disabled sets, each fed a section
/// nothing in this tree can write.
///
/// They are validity checks on a *file*, so the only way to reach them is to
/// build one. The tail is appended by hand onto a section that ends with a zero
/// count, which is exactly the layout `CommandSystem::serialize` writes.
TEST(save_command_section_refuses_a_malformed_disabled_set) {
  CommandSystem writer;
  writer.set_default_verb("idle");
  writer.queue(7).entries.push_back(Command{});
  std::vector<std::byte> base;
  writer.serialize(base);
  // The section ends with the disabled-set count and then the finishing
  // list's count, and this system has neither.
  REQUIRE(base.size() > 8);
  base.resize(base.size() - 8);

  const auto with_tail = [&base](const std::vector<std::byte>& tail) {
    std::vector<std::byte> out = base;
    out.insert(out.end(), tail.begin(), tail.end());
    // No command is finishing.
    for (int i = 0; i < 4; ++i) out.push_back(std::byte{0});
    return out;
  };
  const auto u32 = [](std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
  };
  const auto str = [&u32](std::vector<std::byte>& out, std::string_view text) {
    u32(out, static_cast<std::uint32_t>(text.size()));
    for (const char c : text) out.push_back(static_cast<std::byte>(c));
  };

  CommandSystem target;

  // Names out of order. Iteration order is what gets serialised, so a section
  // that arrived unsorted would answer every question correctly and re-save
  // differently -- which is the one thing a round trip must not do.
  std::vector<std::byte> unsorted;
  u32(unsorted, 1);
  u32(unsorted, 7);
  u32(unsorted, 2);
  str(unsorted, "move");
  str(unsorted, "heal");
  CHECK(target.deserialize(with_tail(unsorted)).error() == FormatError::malformed);

  // A set with nothing in it. The writer never emits one -- `enable_command`
  // drops the row when it empties -- so a file carrying one is a file, not an
  // object that has lost no command.
  std::vector<std::byte> empty;
  u32(empty, 1);
  u32(empty, 7);
  u32(empty, 0);
  CHECK(target.deserialize(with_tail(empty)).error() == FormatError::malformed);

  // Ids out of order, which is the same rule the queues above it follow and for
  // the same reason: `disabled_lower_bound` binary-searches on it.
  std::vector<std::byte> ids;
  u32(ids, 2);
  u32(ids, 9);
  u32(ids, 1);
  str(ids, "move");
  u32(ids, 9);
  u32(ids, 1);
  str(ids, "move");
  CHECK(target.deserialize(with_tail(ids)).error() == FormatError::malformed);

  // And the shape they are all deviations from loads.
  std::vector<std::byte> good;
  u32(good, 1);
  u32(good, 7);
  u32(good, 2);
  str(good, "heal");
  str(good, "move");
  REQUIRE(target.deserialize(with_tail(good)).ok());
  CHECK(target.disabled_commands(7).size() == 2);
}

TEST(save_command_section_round_trips) {
  CommandSystem command;
  command.set_default_verb("idle");
  CommandQueue& queue = command.queue(7);
  Command move;
  move.id = 0x168;
  move.verb = "move";
  move.arg_kind = CommandArgKind::point;
  move.point = Point{640, 480};
  move.user = true;
  move.script = 21;
  move.started = true;
  move.started_at = 4800;
  queue.entries.push_back(move);
  Command enter;
  enter.id = 0x169;
  enter.verb = "enter";
  enter.arg_kind = CommandArgKind::object;
  enter.object = 42;
  enter.param = "Barrack";
  enter.cost_gold = 120;
  enter.delay = 250;
  queue.entries.push_back(enter);
  command.queue(3).entries.push_back(move);

  // The commands two scripts took away. Written last in the section, so a
  // reader of the version-11 layout stops at the header rather than here.
  World world;
  const ObjectId market = world.spawn(NativeClass::building, nullptr);
  const ObjectId temple = world.spawn(NativeClass::building, nullptr);
  CommandTable table;
  CommandDef buy;
  buy.name = "BuySlaves";
  buy.method = "research";
  table.set(buy);
  CommandDef chariot;
  chariot.name = "Chariot of Osiris";
  chariot.method = "research";
  table.set(chariot);
  command.set_table(table);
  REQUIRE(command.disable_command(world, market, "BuySlaves"));
  REQUIRE(command.disable_command(world, temple, "Chariot of Osiris"));
  REQUIRE(command.disable_command(world, temple, "BuySlaves"));

  CommandSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(command, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<CommandSystem>());
  CHECK(fresh.tracked() == 2);
  CHECK(fresh.command_name(7) == "move");
  // The table is deliberately not restored -- it is merged from
  // `DATA/COMMANDS/*.XML` at load -- so the set is checked directly rather than
  // through `command_enabled`, which needs a row to answer at all.
  // `REQUIRE` rather than `CHECK` before every index below: a fault that
  // empties this set must make the suite *fail*, and an unguarded `[0]` on an
  // empty span makes it crash instead -- which reports nothing at all.
  REQUIRE(fresh.disabled_commands(market).size() == 1);
  CHECK(fresh.disabled_commands(market)[0] == "BuySlaves");
  REQUIRE(fresh.disabled_commands(temple).size() == 2);
  // Sorted by the folded name: "buyslaves" before "chariot of osiris".
  CHECK(fresh.disabled_commands(temple)[0] == "BuySlaves");
  CHECK(fresh.disabled_commands(temple)[1] == "Chariot of Osiris");
  // And nothing was invented for the objects that never lost one.
  CHECK(fresh.disabled_commands(7).empty());

  // An emptied set leaves no row in the *bytes* either, which is the only place
  // the difference shows: `disabled_commands` answers an empty span for a row
  // that is still there. Give both commands back and the section is the one a
  // system that never had them writes.
  CommandSystem plain = command;
  REQUIRE(plain.enable_command(market, "BuySlaves"));
  REQUIRE(plain.enable_command(temple, "BuySlaves"));
  REQUIRE(plain.enable_command(temple, "Chariot of Osiris"));
  std::vector<std::byte> given_back;
  plain.serialize(given_back);
  CommandSystem never;
  never.set_default_verb("idle");
  never.queue(7) = *command.find(7);
  never.queue(3) = *command.find(3);
  std::vector<std::byte> untouched;
  never.serialize(untouched);
  CHECK(given_back == untouched);
  CHECK(fresh.command_count(7) == 2);
  const CommandQueue* restored = fresh.find(7);
  REQUIRE(restored != nullptr);
  REQUIRE(restored->entries.size() == 2);
  CHECK(restored->entries[0].script == 21);
  CHECK(restored->entries[0].started);
  CHECK(restored->entries[1].param == "Barrack");
  CHECK(restored->entries[1].object == 42);
}

TEST(save_match_section_round_trips) {
  MatchSystem match;
  MatchRules rules;
  rules.condition = VictoryCondition::elimination;
  rules.param = "1 Elimination";
  rules.start_player = 1;
  rules.map_name = "Numantia";
  rules.map_size = 3;
  match.configure(rules, /*human=*/0, /*multiplayer=*/false);
  MatchPlayer slot;
  slot.control = PlayerControl::computer;
  slot.race = 2;
  slot.participates = true;
  match.set_player(1, slot);
  // `end_game` is a no-op on a slot that takes no part, so the human has to be
  // in the match before it can lose it.
  slot.control = PlayerControl::human;
  slot.race = 0;
  match.set_player(0, slot);
  // The three-argument `EndGame`, whose message is display text and is
  // deliberately not hashed -- and has to survive a save all the same.
  match.end_game(0, /*lost=*/true, "The legions are broken.");
  // Not the default, and not the neighbour of the default: a section that
  // dropped the field would restore 0 and a section that read it at the wrong
  // offset would restore something else, and only a value that is neither
  // separates the two.
  REQUIRE(match.set_difficulty(2));

  MatchSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(match, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<MatchSystem>());
  CHECK(fresh.rules().condition == VictoryCondition::elimination);
  CHECK(fresh.rules().map_name == "Numantia");
  CHECK(fresh.human() == 0);
  CHECK(fresh.player(1).race == 2);
  CHECK(fresh.player(1).participates);
  CHECK(fresh.outcome(0) == MatchOutcome::lost);
  CHECK(fresh.end_message() == "The legions are broken.");
  CHECK(fresh.difficulty() == 2);
}

TEST(save_ai_section_round_trips) {
  AiSystem ai;
  ai.adopt(11, /*player=*/2, /*root=*/true);
  ai.adopt(19, /*player=*/2);
  ai.adopt(23, /*player=*/5);
  ai.set_economy_script(4, 6, 31);
  ai.set_tactic_script(4, 9, 37);
  ai.set_economy_script(1, 2, 41);
  // The ship transport orders, out of order on the way in so that the table's
  // own sort is what the save writes.
  ai.set_ship_transport(9, "advance", Point{640, 960});
  ai.set_ship_transport(4, "move", Point{-3, 7});
  // The manager flag: a loaded match keeps filing placed units into squads.
  ai.start_manager();

  AiSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(ai, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<AiSystem>());
  CHECK(fresh.manager_started());
  CHECK(fresh.owner_entries() == 3);
  REQUIRE(fresh.settlement_scripts().size() == 2);
  // Sorted by settlement id: the vector's order is what `slot` binary-searches.
  CHECK(fresh.settlement_scripts()[0].settlement == 1);
  CHECK(fresh.settlement_scripts()[1].economy == 6);
  CHECK(fresh.settlement_scripts()[1].tactic_script == 37);

  // **The transport orders survive, ascending by ship id**, which is the order
  // the table keeps and therefore the order the bytes have to come back in.
  REQUIRE(fresh.ship_transports().size() == 2);
  CHECK(fresh.ship_transports()[0].ship == 4);
  CHECK(fresh.ship_transports()[1].ship == 9);
  CHECK(fresh.ship_transport(9).order == "advance");
  const Point beach{640, 960};
  const Point odd{-3, 7};
  const Point nowhere{-1, -1};
  CHECK(fresh.ship_transport(9).where == beach);
  CHECK(fresh.ship_transport(4).where == odd);
  // A ship with no row reads as the constructed state rather than as a miss.
  CHECK(fresh.ship_transport(5).order.empty());
  CHECK(fresh.ship_transport(5).where == nowhere);
}

TEST(save_campaign_section_round_trips) {
  // Two territories is enough: what is being tested is the progress vector, and
  // `CampaignSystem::restore` refuses progress whose length disagrees with the
  // conquest the system was configured with.
  constexpr std::string_view kConquest = R"XML(<conquestmap name="Test" choose="1">
    <territory id="Spain" index="3" state="1" bonus="rIberia" neighbours="Gaul"/>
    <territory id="Gaul" index="8" state="0" bonus="rGaul" neighbours="Spain"/>
  </conquestmap>)XML";
  const std::span<const std::byte> xml(reinterpret_cast<const std::byte*>(kConquest.data()),
                                       kConquest.size());
  const Result<ConquestMap> conquest = ConquestMap::parse(xml);
  REQUIRE(conquest.ok());

  CampaignSystem campaign;
  campaign.configure(conquest.value());
  CHECK(campaign.set_state("Spain", TerritoryState::owned));
  campaign.set_active_bonus("rIberia");

  CampaignSystem fresh;
  fresh.configure(conquest.value());
  const std::vector<std::byte> bytes = section_round_trips(campaign, fresh);
  CHECK(!bytes.empty());
  CHECK(fresh.state_of("Spain") == TerritoryState::owned);
  CHECK(fresh.active_bonus() == "rIberia");

  // A save from a different conquest is refused rather than loaded, because
  // `restore` checks the territory count. This is the only system whose
  // section cannot be applied to a differently-configured instance.
  CampaignSystem other;
  CHECK(!other.deserialize(bytes).ok());
}

TEST(save_areas_section_round_trips) {
  // The area table is load-time data -- `load_areas` rebuilds it from
  // `map.obj.xml` on every `GameSession::create` -- and nothing hashes it. It is
  // written anyway, for the same reason `WorldObject::sight` is: recomputing it
  // would make a save's meaning depend on the map data staying put.
  AreaSystem areas;
  CHECK(areas.areas().bind(4, AreaShape::of_circle(Point{500, 600}, 240)));
  CHECK(areas.areas().bind(9, AreaShape::of_rectangle(10, 20, 300, 400)));

  AreaSystem fresh;
  const std::vector<std::byte> bytes = section_round_trips(areas, fresh);
  CHECK(!bytes.empty());
  CHECK(bytes != default_section<AreaSystem>());
  CHECK(fresh.areas().size() == 2);
  const AreaShape* circle = fresh.areas().find(4);
  REQUIRE(circle != nullptr);
  CHECK(circle->kind == AreaKind::circle);
  CHECK(circle->radius == 240);
  CHECK((circle->centre() == Point{500, 600}));
  const AreaShape* box = fresh.areas().find(9);
  REQUIRE(box != nullptr);
  CHECK(box->kind == AreaKind::rectangle);
  CHECK(box->contains(Point{100, 100}));
  CHECK(!box->contains(Point{5, 5}));
}

TEST(save_objlist_pool_round_trips) {
  ObjListPool pool;
  const ObjListId first = pool.acquire(/*script=*/3, /*slot=*/0);
  const ObjListId second = pool.acquire(/*script=*/3, /*slot=*/1);
  const ObjListId third = pool.acquire_temporary(/*script=*/7);
  pool.mutable_items(first)->assign({4, 8, 15});
  pool.mutable_items(third)->assign({16, 23, 42});
  // A hole. Where it is decides which handle the next acquire hands out, so it
  // is state and it has to survive: this is the reason `ObjListPool` needed a
  // serialise pair rather than a wider public API.
  pool.release_script(3);
  const ObjListId fourth = pool.acquire(/*script=*/9, /*slot=*/2);
  CHECK(fourth == first);

  std::vector<std::byte> bytes;
  pool.serialize(bytes);
  ObjListPool restored;
  REQUIRE(restored.deserialize(bytes).ok());
  std::vector<std::byte> again;
  restored.serialize(again);
  CHECK(bytes == again);

  CHECK(restored.capacity() == pool.capacity());
  CHECK(restored.size() == pool.size());
  CHECK(restored.contains(third));
  CHECK(!restored.contains(second));
  CHECK(restored.owner_of(third) == 7);
  REQUIRE(restored.items(third).size() == 3);
  CHECK(restored.items(third)[2] == 42);
  // And the free-list position came with it: the next acquire on the restored
  // pool reuses the same slot the original would have.
  CHECK(restored.acquire(1, 0) == pool.acquire(1, 0));

  ObjListPool target;
  for (std::size_t cut = 0; cut < bytes.size(); cut += 3) {
    CHECK(!target.deserialize(std::span(bytes).first(cut)).ok());
  }
  CHECK(target.capacity() == 0);
}

TEST(save_world_carries_the_objlist_pool) {
  const std::unique_ptr<Game> game = build(0x0B1EC7u);
  ObjListPool& pool = game->world.objlists();
  const ObjListId id = pool.acquire(/*script=*/5, /*slot=*/0);
  pool.mutable_items(id)->assign({game->world.objects()[3].id, game->world.objects()[4].id});

  std::vector<std::byte> data;
  game->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  // Nothing in `World::hashes()` covers this -- `scriptstate` is zero in all
  // nine dumps -- so a load that dropped it would be invisible to every check
  // in this file except this one.
  CHECK(restored.world.objlists().size() == 1);
  REQUIRE(restored.world.objlists().items(id).size() == 2);
  CHECK(restored.world.objlists().items(id)[1] == game->world.objects()[4].id);
}

/// The squad lists survive a save, **cursor and all**.
///
/// Nothing in `World::hashes()` covers this either. The cursor is the half that
/// would go unnoticed: `GS_GUARD.VS` sleeps inside the loop above its walk, so
/// a save can be taken mid-list, and a load that rewound would order every
/// squad in it a second time.
TEST(save_world_carries_the_squadlist_pool_and_its_cursor) {
  const std::unique_ptr<Game> game = build(0x5140057u);
  SquadListPool& pool = game->world.squadlists();
  const SquadListId id = pool.acquire(/*script=*/5, /*slot=*/0);
  pool.mutable_items(id)->assign({SquadKey{3, 1}, SquadKey{4, 1}, SquadKey{1, 2}});
  pool.set_cursor(id, 2);

  std::vector<std::byte> data;
  game->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  REQUIRE(restored.world.squadlists().items(id).size() == 3);
  const SquadKey last{1, 2};
  CHECK(restored.world.squadlists().items(id)[2] == last);
  CHECK(restored.world.squadlists().cursor(id) == 2);
  CHECK(restored.world.squadlists().owner_of(id) == 5);

  // And a truncated payload leaves the pool it was loading into untouched, at
  // every cut -- the rule every other store here follows.
  std::vector<std::byte> bytes;
  pool.serialize(bytes);
  SquadListPool target;
  for (std::size_t cut = 0; cut < bytes.size(); cut += 3) {
    CHECK(!target.deserialize(std::span(bytes).first(cut)).ok());
  }
  CHECK(target.capacity() == 0);
}

/// The conversations survive a save, **cast and all**.
///
/// The half that would go unnoticed is the actors. 63 of the 250 conversation
/// call sites are `SetActor`, and they sit between an `Init` and a `Run` with a
/// `Sleep` or a `WaitIdle` in between often enough that a save can land there:
/// `1_Great_Battles_Zama`'s `seq2.vs` waits ten seconds for a scout to arrive
/// and then binds two roles. A load that came back with the conversation bound
/// and the cast empty would play the same lines with nobody saying them, and
/// nothing in `World::hashes()` covers it -- a script-owned pool is not hashed.
TEST(save_world_carries_the_conversation_pool_and_its_actors) {
  const std::unique_ptr<Game> game = build(0xC0FFEEu);
  ConversationPool& pool = game->world.conversations();
  const ConversationId id = pool.acquire(/*script=*/7, /*slot=*/2);
  CHECK(pool.bind(id, "T_Meeting"));
  CHECK(pool.bind_actor(id, "Scout", game->world.objects()[1].id));
  CHECK(pool.bind_actor(id, "Captain", game->world.objects()[2].id));
  // Rebinding a role replaces it rather than adding a second entry, which is
  // what makes a script that binds the same role twice mean what it says.
  CHECK(pool.bind_actor(id, "Scout", game->world.objects()[3].id));

  std::vector<std::byte> data;
  game->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  const ConversationPool& back = restored.world.conversations();
  CHECK(back.name_of(id) == "T_Meeting");
  CHECK(back.owner_of(id) == 7);
  REQUIRE(back.actors(id).size() == 2);
  CHECK(back.actors(id)[0].role == "Scout");
  CHECK(back.actors(id)[0].object == game->world.objects()[3].id);
  CHECK(back.actors(id)[1].role == "Captain");

  // `Init` clears the cast, so a save taken after a re-`Init` comes back empty
  // rather than carrying the previous conversation's speakers.
  CHECK(pool.bind(id, "T_Bargain"));
  CHECK(pool.actors(id).empty());

  // And a truncated payload leaves the pool it was loading into untouched, at
  // every cut -- the rule every other store here follows.
  std::vector<std::byte> bytes;
  pool.serialize(bytes);
  ConversationPool target;
  for (std::size_t cut = 0; cut < bytes.size(); cut += 3) {
    CHECK(!target.deserialize(std::span(bytes).first(cut)).ok());
  }
  CHECK(target.capacity() == 0);
}

TEST(save_world_carries_the_parry_stance_the_damage_tier_and_the_entering_bit) {
  const std::unique_ptr<Game> game = build(0xBADCAFEu);
  const ObjectId first = game->world.objects()[1].id;
  const ObjectId second = game->world.objects()[2].id;
  // Written straight onto the state rather than through the host entry points:
  // this is a test of the *save*, and the entry points have their own gates.
  game->world.mutable_state(first)->parry_mode = 1;
  game->world.mutable_state(second)->damage_state = 3;
  // And the twenty-first flag bit, which is `Unit::SetEntering`'s.
  game->world.mutable_state(first)->flags.entering = true;

  const std::uint64_t before = game->world.hashes().hash_of_hashes;

  std::vector<std::byte> data;
  game->world.serialize(data);
  Game restored;
  REQUIRE(restored.world.deserialize(data).ok());

  REQUIRE(restored.world.state(first) != nullptr);
  REQUIRE(restored.world.state(second) != nullptr);
  CHECK(restored.world.state(first)->parry_mode == 1);
  CHECK(restored.world.state(second)->damage_state == 3);
  CHECK(restored.world.state(first)->flags.entering);
  // Its neighbours on the packed word came back where they were, which is what
  // a mask one bit out would break.
  CHECK(!restored.world.state(first)->flags.cursed);
  CHECK(!restored.world.state(first)->flags.landing);
  CHECK(restored.world.hashes().hash_of_hashes == before);

  // And both are *hashed*, which is the half a round-trip cannot show: a world
  // that differs only in one of them is a different world.
  game->world.mutable_state(first)->parry_mode = 0;
  CHECK(game->world.hashes().hash_of_hashes != before);
  game->world.mutable_state(first)->parry_mode = 1;
  game->world.mutable_state(second)->damage_state = 2;
  CHECK(game->world.hashes().hash_of_hashes != before);
  game->world.mutable_state(second)->damage_state = 3;
  game->world.mutable_state(first)->flags.entering = false;
  CHECK(game->world.hashes().hash_of_hashes != before);
}

// --------------------------------------------------------------------------
// the two sections the hash cannot see
// --------------------------------------------------------------------------

TEST(save_command_and_ai_are_invisible_to_the_hash_and_caught_by_the_bytes) {
  // The claim this test exists to pin, from docs/formats/save.md: seven of the
  // ten systems fold into `slots`, so losing one is caught at the load by
  // `verify_hashes`. **`command` and `ai` do not.** That asymmetry is the most
  // dangerous thing in the format, and it is asserted here rather than
  // believed: first that the hash really cannot see them, then that the
  // byte-level re-save check really can.
  World world;
  CommandSystem command;
  AiSystem ai;
  world.add_system(&command);
  world.add_system(&ai);
  const ObjectId unit = world.spawn(NativeClass::unit, nullptr);
  world.start();

  Command move;
  move.id = 1;
  move.verb = "move";
  move.arg_kind = CommandArgKind::point;
  move.point = Point{100, 200};
  command.queue(unit).entries.push_back(move);
  ai.adopt(/*script=*/13, /*player=*/2, /*root=*/true);

  // Half of the claim: the world hash is the same with the state and without.
  const std::uint64_t with_state = world.state_hash();
  CommandSystem empty_command;
  AiSystem empty_ai;
  World bare;
  bare.add_system(&empty_command);
  bare.add_system(&empty_ai);
  (void)bare.spawn(NativeClass::unit, nullptr);
  bare.start();
  CHECK(bare.state_hash() == with_state);

  std::vector<std::byte> command_bytes;
  command.serialize(command_bytes);
  std::vector<std::byte> ai_bytes;
  ai.serialize(ai_bytes);
  const SaveSection sections[] = {SaveSection{"command", command_bytes},
                                  SaveSection{"ai", ai_bytes}};
  SaveInputs inputs;
  inputs.systems = sections;
  std::vector<std::byte> data;
  REQUIRE(write_save(world, inputs, data).ok());

  const Result<SaveReader> reader = SaveReader::open(data);
  REQUIRE(reader.ok());

  // A load that applied the world and *not* the two system sections. The hash
  // check passes, which is the danger.
  World target;
  CommandSystem lost_command;
  AiSystem lost_ai;
  target.add_system(&lost_command);
  target.add_system(&lost_ai);
  LoadOptions options;
  const Result<LoadReport> report = read_save(reader.value(), target, options);
  REQUIRE(report.ok());
  CHECK(report->unrestored_systems.empty());  // the sections are there
  CHECK(verify_hashes(target, reader->meta()).ok);
  CHECK(lost_command.tracked() == 0);  // ...and were not applied
  CHECK(lost_ai.owner_entries() == 0);

  // The other half: re-saving the target and comparing the bytes does catch it.
  // This is the mechanism `engine/tools/imsave.cpp` uses over a real map, and
  // it is the only one that sees these two.
  std::vector<std::byte> lost_command_bytes;
  lost_command.serialize(lost_command_bytes);
  std::vector<std::byte> lost_ai_bytes;
  lost_ai.serialize(lost_ai_bytes);
  CHECK(lost_command_bytes != command_bytes);
  CHECK(lost_ai_bytes != ai_bytes);

  // And an honest load reproduces both exactly.
  REQUIRE(lost_command.deserialize(reader->system_section("command")).ok());
  REQUIRE(lost_ai.deserialize(reader->system_section("ai")).ok());
  std::vector<std::byte> again;
  lost_command.serialize(again);
  CHECK(again == command_bytes);
  again.clear();
  lost_ai.serialize(again);
  CHECK(again == ai_bytes);
}

}  // namespace
