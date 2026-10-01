# stb_vorbis, Sean Barrett's one-file Ogg Vorbis decoder, vendored unchanged
# (README.md says from where, LICENSE under what). It decodes the
# installation's music/*.ogg for engine/sound, and nothing else links it:
# never engine/core, which `tools/check_core_boundary.py` keeps freestanding.
#
# Its own library, so its warnings and its C stay here: it is compiled with
# warnings off, and its header reaches the one file that includes it as a
# system header, so nothing of it shows in this project's own diagnostics.

add_library(stb_vorbis STATIC stb_vorbis.c)
set_target_properties(stb_vorbis PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
target_include_directories(stb_vorbis SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
# From memory only: no FILE* entry points, and no push API. Public, because the
# declarations a consumer sees must be the ones the library was built with.
target_compile_definitions(stb_vorbis PUBLIC STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API)
if(MSVC)
  target_compile_options(stb_vorbis PRIVATE /w)
else()
  target_compile_options(stb_vorbis PRIVATE -w)
endif()
