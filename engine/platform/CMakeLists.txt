# imperivm_platform owns everything core is forbidden to touch: windowing,
# input, the audio device, the GPU, the clock, and the filesystem. It depends
# on core, and on engine/sound for the mixer the device plays;
# core must never depend on it.

file(GLOB_RECURSE IMPERIVM_PLATFORM_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(imperivm_platform STATIC ${IMPERIVM_PLATFORM_SOURCES})
target_include_directories(imperivm_platform PUBLIC include)
target_link_libraries(imperivm_platform PUBLIC imperivm_core imperivm_sound)

if(EMSCRIPTEN)
  # Emscripten ships its own SDL; ask for the version that matches.
  target_compile_options(imperivm_platform PUBLIC "-sUSE_SDL=3")
  target_link_options(imperivm_platform PUBLIC "-sUSE_SDL=3")
else()
  find_package(SDL3 CONFIG REQUIRED)
  target_link_libraries(imperivm_platform PUBLIC SDL3::SDL3)
endif()
