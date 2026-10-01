cmake_minimum_required(VERSION 3.24)
# C for the one vendored C source, engine/third_party/stb_vorbis.
project(imperivm_reforged LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# RelWithDebInfo rather than Debug, because the default build is the one people
# play. A simulation turn on Numantia costs about 16 ms unoptimised and about
# 2 ms at -O2: at the shipped ten turns a second that is the difference between
# a sixth of the frame budget and a sixtieth, and anything that asks the
# simulation to run faster falls off a cliff unoptimised and does not
# optimised. -g is still on, NDEBUG only removes `assert`, which nothing here
# uses -- the test harness has its own CHECK. Configure with
# -DCMAKE_BUILD_TYPE=Debug when you want a debugger to make sense of a frame.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(IMPERIVM_BUILD_TESTS "Build the core test binary" ON)
option(IMPERIVM_BUILD_APP   "Build the windowed application" ON)
option(IMPERIVM_BUILD_TOOLS "Build imcheck, the cross-validation harness" ON)

# Determinism: the simulation must behave identically everywhere. Fast-math and
# x87 excess precision are the two classic ways that silently stops being true.
add_library(imperivm_determinism INTERFACE)
if(NOT MSVC)
  target_compile_options(imperivm_determinism INTERFACE
    -ffp-contract=off -fno-fast-math)
endif()

add_subdirectory(engine/core)
add_subdirectory(engine/gamedata)
add_subdirectory(engine/third_party/stb_vorbis)
add_subdirectory(engine/sound)
if(NOT EMSCRIPTEN)
  add_subdirectory(engine/net)
endif()
if(IMPERIVM_BUILD_APP)
  add_subdirectory(engine/platform)
  add_subdirectory(engine/app)
endif()
if(IMPERIVM_BUILD_TOOLS)
  add_subdirectory(engine/tools)
endif()
if(IMPERIVM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(engine/tests)
endif()
