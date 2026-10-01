#pragma once

// A test harness small enough to read in one sitting. Registration happens at
// static-init time; TEST(name) declares a case, CHECK(expr) asserts inside one.

#include <cstdio>
#include <vector>

namespace imperivm::test {

extern int failures;
extern int checks;

using Fn = void (*)();

struct Case {
  const char* name;
  Fn fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Register {
  Register(const char* name, Fn fn) { registry().push_back({name, fn}); }
};

inline void run_all(bool verbose = false) {
  for (const auto& c : registry()) {
    if (verbose) {
      // Flushed before the call so that a crash names the test that caused it.
      std::printf("run %s\n", c.name);
      std::fflush(stdout);
    }
    const int before = failures;
    c.fn();
    if (failures != before) std::printf("FAIL %s\n", c.name);
  }
}

inline bool check(bool ok, const char* expr, const char* file, int line) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("  %s:%d: %s\n", file, line, expr);
  }
  return ok;
}

}  // namespace imperivm::test

#define TEST(name)                                                        \
  static void name();                                                     \
  static const ::imperivm::test::Register register_##name{#name, &name};  \
  static void name()

/// Records a failure and carries on.
#define CHECK(expr) ((void)::imperivm::test::check((expr), #expr, __FILE__, __LINE__))

/// Records a failure and abandons the rest of the test.
///
/// Use this for preconditions. A CHECK that a container has two entries does
/// not stop the next line from indexing it, so a failed precondition turns
/// into a segfault that hides the real failure -- which is exactly what
/// happened here before REQUIRE existed.
#define REQUIRE(expr)                                                     \
  do {                                                                    \
    if (!::imperivm::test::check((expr), "REQUIRE(" #expr ")", __FILE__,  \
                                 __LINE__))                               \
      return;                                                             \
  } while (0)
