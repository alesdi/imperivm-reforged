# imperivm_net owns the socket and the clock a networked match needs, and
# nothing else. Everything a peer *decides* -- the turn packet, the link layer,
# the lobby messages -- is in engine/core, where it runs on a loopback in the
# core tests. This is the half that has to touch the operating system.
#
# It depends on core and on the system's sockets, not on SDL, so the headless
# tools can link it from the dependency-free build. There is no UDP in a
# browser, so the WebAssembly build has no net library; a WebRTC transport
# would be the one to write there.

file(GLOB_RECURSE IMPERIVM_NET_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(imperivm_net STATIC ${IMPERIVM_NET_SOURCES})
target_include_directories(imperivm_net PUBLIC include)
target_link_libraries(imperivm_net PUBLIC imperivm_core)
if(WIN32)
  target_link_libraries(imperivm_net PRIVATE ws2_32)
endif()

# The lobby and the match over real sockets, on loopback, in one process.
# Separate from core_tests on purpose: those may not open a socket.
if(IMPERIVM_BUILD_TESTS)
  add_executable(net_tests tests/net_tests.cpp)
  target_include_directories(net_tests PRIVATE ${PROJECT_SOURCE_DIR}/engine/tests)
  target_link_libraries(net_tests PRIVATE imperivm_net)
  add_test(NAME net_tests COMMAND net_tests)
endif()
