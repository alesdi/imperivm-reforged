// Core tests. Deliberately dependency-free: a test binary that needs a package
// manager is a test binary that stops being run.

#include <cstdio>

#include "imperivm/core/version.hpp"
#include "test.hpp"

namespace imperivm::test {

int failures = 0;
int checks = 0;

}  // namespace imperivm::test

TEST(core_reports_its_version) {
  CHECK(imperivm::core::version_string() != nullptr);
}

int main(int argc, char**) {
  imperivm::test::run_all(argc > 1);
  std::printf("%d checks, %d failures\n", imperivm::test::checks, imperivm::test::failures);
  return imperivm::test::failures == 0 ? 0 : 1;
}
