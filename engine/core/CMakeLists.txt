# imperivm_core is FREESTANDING. It must not depend on SDL, on the filesystem,
# on the clock, on threads, or on any platform service. It transforms bytes and
# state into state. See docs/engine/architecture.md.
#
# This is not architectural purity for its own sake: the conformance harness
# replays recorded command streams headlessly and compares per-tick state
# hashes against the original engine's desync dumps. That harness cannot exist
# unless core builds and runs with no platform underneath it. Portability to
# WebAssembly, iOS and Android then falls out at no extra cost.

file(GLOB_RECURSE IMPERIVM_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(imperivm_core STATIC ${IMPERIVM_CORE_SOURCES})
target_include_directories(imperivm_core PUBLIC include)
target_link_libraries(imperivm_core PUBLIC imperivm_determinism)

# Deliberately links nothing else. If you find yourself wanting to add a
# dependency here, the code belongs in engine/platform instead.

# Opt-in: check every spatial query's answer against the straight scan it
# replaced, and trap on the first disagreement (`World::sweep`). It costs the
# whole scan the grid saves and more -- it made a Debug Crossroads run eight
# times slower -- so it is not tied to Debug. core_tests' randomised oracle
# checks the grid against the scan in every build; turn this on when the
# index itself changes: -DIMPERIVM_SPATIAL_CHECK=ON.
option(IMPERIVM_SPATIAL_CHECK "Check every spatial query against the straight scan" OFF)
if(IMPERIVM_SPATIAL_CHECK)
  target_compile_definitions(imperivm_core PRIVATE IMPERIVM_SPATIAL_CHECK)
endif()
