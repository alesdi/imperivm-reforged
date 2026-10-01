# imperivm_sound decides what is heard: it decodes the installation's WAVs,
# reads its sound entities, picks a variant, and mixes voices into a stereo
# buffer. It does not open a device -- engine/platform's `Audio` hands the
# mixer's output to SDL -- so it builds, and its tests run, without SDL, in the
# dependency-free build.
#
# It depends on core for the XML reader, and privately on the vendored
# stb_vorbis for the music's Ogg Vorbis (engine/third_party), so nothing above
# it sees the decoder. Core never depends on it: sound is presentation, never
# simulation state, and nothing here is hashed or saved. See
# docs/engine/sound.md.

file(GLOB_RECURSE IMPERIVM_SOUND_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(imperivm_sound STATIC ${IMPERIVM_SOUND_SOURCES})
target_include_directories(imperivm_sound PUBLIC include)
target_link_libraries(imperivm_sound PUBLIC imperivm_core PRIVATE stb_vorbis)

# The WAV decoder, the entity reader and the mixer's arithmetic, on synthetic
# buffers. Separate from core_tests, which link core alone.
if(IMPERIVM_BUILD_TESTS)
  add_executable(sound_tests tests/sound_tests.cpp)
  target_include_directories(sound_tests PRIVATE ${PROJECT_SOURCE_DIR}/engine/tests)
  target_link_libraries(sound_tests PRIVATE imperivm_sound)
  add_test(NAME sound_tests COMMAND sound_tests)
endif()
