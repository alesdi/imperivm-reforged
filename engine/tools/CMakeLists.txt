# imcheck runs the core readers over a real installation and prints what they
# found, so the output can be diffed against the Python reference readers in
# src/imperivm/formats/. Those are validated across the whole retail corpus, so
# they are the ground truth a C++ port has to match.
#
# It lives here rather than in core because it opens files, which core may not.
add_executable(imcheck imcheck.cpp)
target_link_libraries(imcheck PRIVATE imperivm_core)

# imentity is the same idea for the object model: entity definitions and the
# native class registry, checked against gamedata.py over a real installation.
# A separate binary so that the cross-validation harness can be extended from
# two directions at once without merging a verification tool.
add_executable(imentity imentity.cpp)
target_link_libraries(imentity PRIVATE imperivm_core)

# imrun loads a retail map, starts every object's own script, and reports the
# traps grouped by cause. It is the falsifier for the host coverage number: that
# figure counts entry points that exist, and this counts the ones a shipped
# script actually reaches.
add_executable(imrun imrun.cpp)
target_link_libraries(imrun PRIVATE imperivm_core imperivm_gamedata)

# imconform is the determinism harness's front end: it runs the self-consistency,
# partition-invariance, reserved-channel and golden-trace checks over either the
# built-in synthetic scenario or a real session built from a shipped map. The
# harness itself is in engine/core, where it runs with no platform beneath it;
# this half opens the installation and writes the golden file.
add_executable(imconform imconform.cpp)
# And the installation loader, for --skirmish: a skirmish is the game only with
# the terrain, the settlement templates and the entities, which is what
# imperivm_gamedata assembles for imrun and the app.
target_link_libraries(imconform PRIVATE imperivm_core imperivm_gamedata)
# And the socket, for udp-host and udp-join -- which is why imconform is the one
# tool that links imperivm_net. Not on WebAssembly, where there is no UDP.
if(TARGET imperivm_net)
  target_link_libraries(imconform PRIVATE imperivm_net)
  target_compile_definitions(imconform PRIVATE IMPERIVM_HAVE_NET=1)
endif()

# imsave is the save/load harness's front end: it runs a real session over a
# shipped map, saves it, loads it into a fresh session and requires the two to
# be the same game -- at every turn, and byte for byte on a re-save. The core
# tests link `imperivm_core` alone and so cannot open a map; this half can.
add_executable(imsave imsave.cpp)
target_link_libraries(imsave PRIVATE imperivm_core imperivm_gamedata)

# immap is the map-container writer's front end: it rewrites one container
# with its object list regenerated, so that the corpus test can run the game
# over the original and the copy and require the same hash. The editor's Save
# is the same call with an edited list.
add_executable(immap immap.cpp)
target_link_libraries(immap PRIVATE imperivm_core imperivm_gamedata)
