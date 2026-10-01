# imperivm_gamedata opens a retail installation: the pack, the class graph, a
# map container, and the byte spans a `sim::GameSession` is built from.
#
# It is its own target rather than part of `imperivm_platform` for one reason:
# **platform links SDL publicly**, and the headless tools must keep building
# without it. `docs/plan.html` calls the dependency-free core build the one that
# must never break, and a loader that dragged a windowing library into
# `imrun` would break it.
#
# It depends on core alone. It opens files, which core may not, and that is the
# whole of what it adds.
file(GLOB_RECURSE IMPERIVM_GAMEDATA_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(imperivm_gamedata STATIC ${IMPERIVM_GAMEDATA_SOURCES})
target_include_directories(imperivm_gamedata PUBLIC include)
target_link_libraries(imperivm_gamedata PUBLIC imperivm_core)
