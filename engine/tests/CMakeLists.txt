# Core tests link imperivm_core only. If a test needs a file or a window, it is
# testing the wrong layer.
file(GLOB_RECURSE IMPERIVM_TEST_SOURCES CONFIGURE_DEPENDS *.cpp)

add_executable(core_tests ${IMPERIVM_TEST_SOURCES})
target_link_libraries(core_tests PRIVATE imperivm_core)
add_test(NAME core_tests COMMAND core_tests)
