// `IntArray` and `StrArray`: sim/array.hpp.

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A world, the whole host surface and a scheduler -- because the interesting
/// tests are the ones that run the shipped idiom rather than call the pool.
struct ArrayBench {
  World world;
  EnvSystem env;
  script::HostRegistry registry;
  HostContext context;
  WorldHost host{world};
  script::Scheduler scheduler;

  ArrayBench() {
    world.add_system(&env);
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    scheduler.set_user(&context);
    install_objlist_lifetime(scheduler);
  }

  /// Compile and start one script, saying why if it will not build. A silently
  /// unbuilt chunk would make every case here pass by never running.
  script::ScriptId run(std::string_view source, const char* name) {
    script::Diagnostic diagnostic;
    const auto parsed = script::parse(bytes_of(source), name, &diagnostic);
    if (!parsed.ok()) {
      std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                  static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
      return script::kNoScript;
    }
    script::CompileError error;
    auto chunk = script::compile(parsed.value(), &registry, &error);
    if (!chunk.ok()) {
      std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
      return script::kNoScript;
    }
    return scheduler.spawn(scheduler.add_chunk(std::move(chunk.value())));
  }

  [[nodiscard]] std::int32_t env_int(const char* key) const {
    return env.env().read_int(EnvScope::root(), key);
  }
};

}  // namespace

/// The pool's own contract: grow on write, zero-fill the gap, and answer the
/// element type's zero past the end without growing.
TEST(array_writing_past_the_end_grows_and_fills_with_the_zero_value) {
  ArrayPool pool;
  const ScriptArrayId ints = pool.acquire(1, 0, /*strings=*/false);
  REQUIRE(ints != kNoScriptArray);
  CHECK(pool.size(ints) == 0);

  // Out of order, so the fill is visible: writing element 3 makes 0, 1 and 2.
  CHECK(pool.set(ints, 3, script::Value::integer(7)));
  CHECK(pool.size(ints) == 4);
  CHECK(pool.get(ints, 3).as_integer() == 7);
  for (std::int32_t i = 0; i < 3; ++i) CHECK(pool.get(ints, i).as_integer() == 0);

  // Writing inside does not shrink or grow.
  CHECK(pool.set(ints, 1, script::Value::integer(5)));
  CHECK(pool.size(ints) == 4);
  CHECK(pool.get(ints, 1).as_integer() == 5);

  // Past the end is the zero value, and the array is not extended by asking.
  CHECK(pool.get(ints, 99).as_integer() == 0);
  CHECK(pool.size(ints) == 4);

  const ScriptArrayId text = pool.acquire(1, 1, /*strings=*/true);
  CHECK(pool.set(text, 2, script::Value::string("Free Beer")));
  CHECK(pool.size(text) == 3);
  CHECK(pool.get(text, 2).as_string() == "Free Beer");
  CHECK(pool.get(text, 0).as_string().empty());
  CHECK(pool.get(text, 50).as_string().empty());
}

/// A negative index is refused on both sides, and the ceiling is a refusal too.
///
/// There is no element -1 to answer with and no length that could hold one; and
/// an uncapped `a[2000000000] = 1` is a script's typo turned into an allocation
/// failure, which is not identical on every peer the way a refusal is.
TEST(array_a_negative_index_and_the_ceiling_are_both_refusals) {
  ArrayPool pool;
  const ScriptArrayId ints = pool.acquire(1, 0, false);
  CHECK(!pool.set(ints, -1, script::Value::integer(1)));
  CHECK(!pool.set(ints, -2147483647 - 1, script::Value::integer(1)));
  CHECK(pool.size(ints) == 0);
  CHECK(pool.get(ints, -1).as_integer() == 0);

  CHECK(!pool.set(ints, kMaxElements, script::Value::integer(1)));
  CHECK(!pool.set(ints, 2000000000, script::Value::integer(1)));
  CHECK(pool.size(ints) == 0);
  // And the last index it will hold does work, so the bound is off by nothing.
  CHECK(pool.set(ints, kMaxElements - 1, script::Value::integer(3)));
  CHECK(pool.size(ints) == static_cast<std::size_t>(kMaxElements));
}

/// An entry's element type is fixed by the declaration, and a value of the
/// other type is refused rather than converted.
TEST(array_an_entry_holds_one_element_type) {
  ArrayPool pool;
  const ScriptArrayId ints = pool.acquire(1, 0, false);
  const ScriptArrayId text = pool.acquire(1, 1, true);
  CHECK(!pool.set(ints, 0, script::Value::string("no")));
  CHECK(!pool.set(text, 0, script::Value::integer(1)));
  CHECK(pool.size(ints) == 0);
  CHECK(pool.size(text) == 0);
  CHECK(pool.set(ints, 0, script::Value::integer(1)));
  CHECK(pool.set(text, 0, script::Value::string("yes")));
}

/// The same declaration site always names the same entry, cleared -- which is
/// what bounds the pool by the program text rather than by how long a script
/// runs, and what `ObjListPool` already does for the same reason.
TEST(array_re_entering_a_declaration_clears_and_reuses_one_entry) {
  ArrayPool pool;
  const ScriptArrayId first = pool.acquire(4, 2, false);
  CHECK(pool.set(first, 5, script::Value::integer(9)));
  CHECK(pool.size(first) == 6);

  const ScriptArrayId again = pool.acquire(4, 2, false);
  CHECK(again == first);
  CHECK(pool.size(again) == 0);
  CHECK(pool.capacity() == 1);

  // A different slot is a different entry, and a different script is too.
  CHECK(pool.acquire(4, 3, false) != first);
  CHECK(pool.acquire(5, 2, false) != first);
  CHECK(pool.capacity() == 3);

  // And a dead script's entries go, leaving a handle that reads as empty rather
  // than as somebody else's.
  pool.release_script(4);
  CHECK(!pool.contains(first));
  CHECK(pool.size(first) == 0);
  CHECK(pool.get(first, 0).as_integer() == 0);
  CHECK(!pool.set(first, 0, script::Value::integer(1)));
  // The slot is reused before the pool grows, so a handle a save wrote is the
  // index it reads back.
  CHECK(pool.acquire(9, 0, false) == first);
  CHECK(pool.capacity() == 3);
}

/// The pool round-trips, dead slots and all.
///
/// **Not hashed, and that is the standing decision**: `scriptstate` is zero in
/// all nine desync dumps and `ObjListPool` is saved and unhashed for exactly
/// that reason. Which is precisely why it has to be *saved* -- nothing
/// downstream would notice its absence.
TEST(array_the_pool_round_trips_including_its_holes) {
  ArrayPool pool;
  const ScriptArrayId a = pool.acquire(1, 0, false);
  const ScriptArrayId b = pool.acquire(1, 1, true);
  const ScriptArrayId c = pool.acquire(2, 0, false);
  CHECK(pool.set(a, 2, script::Value::integer(-7)));
  CHECK(pool.set(b, 1, script::Value::string("Free Wine")));
  // The **last** entry is live and carries elements, deliberately: a
  // truncation lands inside its element list, which is the only place an
  // element loop that stopped early instead of refusing could apply a
  // half-read pool. With the hole last, every cut lands in a header field and
  // the next header read refuses on its own -- so the fault survived.
  CHECK(pool.set(c, 3, script::Value::integer(11)));
  const ScriptArrayId d = pool.acquire(3, 0, false);
  CHECK(pool.set(d, 1, script::Value::integer(2)));
  pool.release_script(1);  // leaves two holes, at `a` and `b`

  std::vector<std::byte> bytes;
  pool.serialize(bytes);
  ArrayPool loaded;
  REQUIRE(loaded.deserialize(bytes).ok());

  CHECK(loaded.capacity() == pool.capacity());
  CHECK(!loaded.contains(a));
  CHECK(!loaded.contains(b));
  CHECK(loaded.contains(c));
  CHECK(loaded.contains(d));
  CHECK(loaded.size(c) == 4);
  CHECK(loaded.get(c, 3).as_integer() == 11);
  CHECK(loaded.get(c, 0).as_integer() == 0);
  CHECK(loaded.get(d, 1).as_integer() == 2);
  CHECK(loaded.owner_of(c) == 2);
  // A released entry comes back released, with nothing in it.
  CHECK(loaded.size(a) == 0);

  // **A truncated save leaves the pool it was loading into untouched**, at every
  // length rather than one. Cutting the last byte lands in a length field;
  // cutting further lands inside an element list, and an element loop that
  // stopped early instead of refusing would apply a half-read pool and pass a
  // test that only ever cut one byte. It did.
  for (std::size_t cut = 1; cut + 8 < bytes.size(); ++cut) {
    ArrayPool survivor;
    (void)survivor.acquire(3, 0, false);
    CHECK(survivor.set(1, 0, script::Value::integer(42)));
    const std::vector<std::byte> short_save(bytes.begin(), bytes.end() - cut);
    CHECK(!survivor.deserialize(short_save).ok());
    CHECK(survivor.capacity() == 1);
    CHECK(survivor.contains(1));
    CHECK(survivor.get(1, 0).as_integer() == 42);
    CHECK(survivor.owner_of(1) == 3);
  }
}

/// **The shipped idiom, compiled and run.**
///
/// Four sequences open with exactly this -- an array declared empty and written
/// past its end -- and every one of them trapped with *"host refused this
/// subscript assignment"* until the type existed. `ESH_MARKET.VS` does the
/// string half with eight upgrade names and reads them back in a loop.
TEST(array_the_shipped_idiom_compiles_and_runs) {
  ArrayBench b;
  const script::ScriptId script = b.run(
      "//void\n"
      "int n_Count;\n"
      "IntArray nA_Conditions;\n"
      "StrArray strTechMarket;\n"
      "for (n_Count = 0; n_Count < 4; n_Count += 1)\n"
      "  nA_Conditions[n_Count] = 0;\n"
      "nA_Conditions[2] = 7;\n"
      "strTechMarket[0] = \"Free Beer\";\n"
      "strTechMarket[1] = \"Gambling\";\n"
      "if (nA_Conditions[2] == 7 && nA_Conditions[3] == 0)\n"
      "  EnvWriteInt(\"/ints\", 1);\n"
      "if (strTechMarket[1] == \"Gambling\" && strTechMarket[0] != \"\")\n"
      "  EnvWriteInt(\"/text\", 1);\n",
      "arrays.vs");
  REQUIRE(script != script::kNoScript);
  b.scheduler.advance(10);
  CHECK(b.env_int("/ints") == 1);
  CHECK(b.env_int("/text") == 1);
  // No trap: the whole script ran to the end.
  CHECK(!b.scheduler.alive(script));
}

/// A script's arrays go when the script does, through the same teardown hook
/// that releases its `ObjList`s -- one hook, because the scheduler has one and
/// because the two are released on precisely the same event.
TEST(array_a_dead_scripts_arrays_are_released_with_its_lists) {
  ArrayBench b;
  REQUIRE(b.run("//void\nIntArray a;\na[3] = 1;\n", "short.vs") != script::kNoScript);
  b.scheduler.advance(10);
  CHECK(b.world.arrays().capacity() == 1);
  // The script finished on that pass; compaction and teardown run at its end.
  CHECK(!b.world.arrays().contains(1));
}
