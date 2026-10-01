// Native class registry tests.
//
// The registry is stubs, so there is no behaviour to test yet. What these pin
// down is the shape: that every `cpp_class` the shipped data can name resolves,
// that the hierarchy the class graph implies is the hierarchy the C++ types
// have, and that the nine classes nothing is known about are labelled as such
// rather than quietly given a responsibility somebody later builds on.

#include <string_view>

#include "imperivm/core/game/registry.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

/// Every `cpp_class` value in the retail data, with the census measured by the
/// reference loader over the shipped packs. All 845 classes name one of these
/// 26, so the counts must sum to 845 — which is how the six rows that
/// docs/data-model.md undercounts were found.
constexpr struct {
  std::string_view name;
  int classes;
} kCorpus[] = {
    {"CVXDecor", 148},    {"CVXUnit", 134},        {"CVXBuilding", 134},
    {"CVXHero", 114},     {"CVXGate", 65},         {"CVXFeedback", 47},
    {"CVXScriptObj", 37}, {"CVXTownHall", 41},     {"CVXItemHolder", 23},
    {"CVXBarrack", 23},   {"CVXMapObj", 12},       {"CVXTavern", 12},
    {"CVXDruid", 9},      {"CVXCatapult", 8},      {"CVXWagon", 8},
    {"CVXOutpost", 9},    {"CVXShip", 4},          {"CVXCatapultShot", 3},
    {"CVXFlyingUnit", 3}, {"CVXTeleport", 3},      {"CVXGhost", 2},
    {"CVXSacrifice", 2},  {"CVXAdvArea", 1},       {"CVXArea", 1},
    {"CVXAreaEffect", 1}, {"CVXDestLock", 1},
};

constexpr std::size_t kCorpusSize = sizeof(kCorpus) / sizeof(kCorpus[0]);

}  // namespace

TEST(registry_resolves_every_shipped_cpp_class) {
  REQUIRE(kCorpusSize == kNativeClassCount);
  REQUIRE(native_classes().size() == kNativeClassCount);

  int total = 0;
  for (const auto& expected : kCorpus) {
    const auto id = native_class_from_name(expected.name);
    CHECK(id.ok());
    if (!id.ok()) continue;
    const NativeClassInfo& info = native_class_info(*id);
    CHECK(info.name == expected.name);
    CHECK(info.corpus_classes == expected.classes);
    total += expected.classes;
  }
  // The census has to add up to the whole corpus, or a class is bound to
  // something this registry does not know about.
  CHECK(total == 845);
}

TEST(registry_rejects_a_name_it_does_not_know) {
  // Exact matching on purpose: a near miss in shipped data would be a finding,
  // and papering over it here is how a typo becomes permanent.
  CHECK(!native_class_from_name("CVXUnknown").ok());
  CHECK(!native_class_from_name("cvxunit").ok());
  CHECK(!native_class_from_name("CVXUnit ").ok());
  CHECK(!native_class_from_name("").ok());
  CHECK(native_class_from_name("CVXUnknown").error() == FormatError::not_found);
  CHECK(make_native_object("CVXUnknown") == nullptr);
}

TEST(registry_table_is_indexed_by_its_enumerator) {
  const auto table = native_classes();
  REQUIRE(table.size() == kNativeClassCount);
  for (std::size_t i = 0; i < table.size(); ++i) {
    CHECK(static_cast<std::size_t>(table[i].id) == i);
    CHECK(!table[i].name.empty());
    CHECK(!table[i].summary.empty());
  }
}

TEST(registry_reproduces_the_native_hierarchy) {
  // The `cpp_class` assignment follows the inheritance tree, so the 26 names
  // are the original engine's C++ hierarchy showing through the data.
  // Behaviour added to a base in Part 5 has to land on every class the data
  // says inherits it.
  CHECK(native_class_is_a(NativeClass::outpost, NativeClass::town_hall));
  CHECK(native_class_is_a(NativeClass::outpost, NativeClass::building));
  CHECK(native_class_is_a(NativeClass::outpost, NativeClass::decor));
  CHECK(native_class_is_a(NativeClass::hero, NativeClass::unit));
  CHECK(native_class_is_a(NativeClass::adv_area, NativeClass::area));
  CHECK(native_class_is_a(NativeClass::area_effect, NativeClass::script_obj));
  CHECK(native_class_is_a(NativeClass::gate, NativeClass::building));
  // A catapult is a building, which the class tree insists on and gameplay
  // does not suggest.
  CHECK(native_class_is_a(NativeClass::catapult, NativeClass::building));

  CHECK(!native_class_is_a(NativeClass::unit, NativeClass::building));
  CHECK(!native_class_is_a(NativeClass::building, NativeClass::unit));
  CHECK(!native_class_is_a(NativeClass::town_hall, NativeClass::outpost));
  CHECK(!native_class_is_a(NativeClass::feedback, NativeClass::script_obj));

  // Everything is a decor, and decor is the root: its parent is itself, so the
  // walk terminates without a null case.
  for (const NativeClassInfo& info : native_classes()) {
    CHECK(native_class_is_a(info.id, info.id));
    CHECK(native_class_is_a(info.id, NativeClass::decor));
  }
  CHECK(native_class_info(NativeClass::decor).parent == NativeClass::decor);
}

TEST(registry_constructs_every_native_type) {
  for (const NativeClassInfo& info : native_classes()) {
    const auto object = make_native_object(info.id);
    REQUIRE(object != nullptr);
    // The dynamic type has to report the enumerator it was made from, or the
    // factory and the table have silently drifted apart.
    CHECK(object->native_class() == info.id);
    CHECK(object->native_class_name() == info.name);
    CHECK(object->is_a(NativeClass::decor));
  }
}

TEST(registry_constructs_from_a_cpp_class_name) {
  const auto hero = make_native_object("CVXHero");
  REQUIRE(hero != nullptr);
  CHECK(hero->native_class() == NativeClass::hero);
  CHECK(hero->is_a(NativeClass::unit));
  CHECK(!hero->is_a(NativeClass::building));
}

TEST(native_objects_start_in_a_defined_state) {
  const auto object = make_native_object(NativeClass::unit);
  REQUIRE(object != nullptr);
  CHECK(object->id == kNoObject);
  CHECK(object->class_index == kNoClass);
  CHECK(object->owner == kNoPlayer);
  CHECK(object->alive);
  CHECK(object->entity == nullptr);
  CHECK(object->position.x == 0 && object->position.y == 0 && object->position.z == 0);
  // Every entity declares state 1; nothing declares an animation until one is
  // played, and "no animation" is the format's own 0x10000 sentinel.
  CHECK(object->anim.state_idx == 1);
  CHECK(object->anim.anim_slot == kNoAnim);
  CHECK(object->anim.elapsed_ms == 0);
  CHECK(object->anim.variation == 0);
}

TEST(native_behaviour_hooks_are_deliberate_no_ops) {
  // Part 5 fills these in. The point of calling them now is that the call
  // sites the loader and tick loop need can be written against something real.
  const auto object = make_native_object(NativeClass::building);
  REQUIRE(object != nullptr);
  object->on_spawn();
  object->on_tick(0);
  object->on_tick(1);
  object->on_destroy();
  CHECK(object->alive);
}

TEST(registry_admits_what_it_does_not_know) {
  // Nine native classes add no property, method or behaviour anywhere in their
  // subtree, so the data says nothing about what they do. Marking them is the
  // difference between a gap somebody closes and a guess somebody builds on.
  constexpr std::string_view kUnknown[] = {
      "CVXMapObj",   "CVXWagon", "CVXSacrifice",    "CVXArea",     "CVXAdvArea",
      "CVXDestLock", "CVXGhost", "CVXCatapultShot", "CVXFeedback",
  };

  int uninvestigated = 0;
  for (const NativeClassInfo& info : native_classes()) {
    if (info.confidence == NativeClassConfidence::uninvestigated) ++uninvestigated;
  }
  CHECK(uninvestigated == 9);

  for (const std::string_view name : kUnknown) {
    const auto id = native_class_from_name(name);
    CHECK(id.ok());
    if (!id.ok()) continue;
    CHECK(native_class_info(*id).confidence == NativeClassConfidence::uninvestigated);
  }
  CHECK(native_class_info(NativeClass::hero).confidence ==
        NativeClassConfidence::characterised);
}
