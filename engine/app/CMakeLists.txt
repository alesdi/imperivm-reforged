add_executable(imperivm main.cpp)
target_link_libraries(imperivm PRIVATE imperivm_platform imperivm_gamedata)
# The socket, for --host and --join. Not on the web, which has no UDP.
if(TARGET imperivm_net)
  target_link_libraries(imperivm PRIVATE imperivm_net)
  target_compile_definitions(imperivm PRIVATE IMPERIVM_HAVE_NET=1)
endif()

if(EMSCRIPTEN)
  set_target_properties(imperivm PROPERTIES SUFFIX ".html")
  target_link_options(imperivm PRIVATE
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMAX_WEBGL_VERSION=2")
endif()
