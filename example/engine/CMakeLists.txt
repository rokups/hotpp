# Engine harness. ENABLE_EXPORTS adds -rdynamic, so a generation module
# resolves the engine symbols and the runtime symbols from the executable.
# The file watcher and the command channel are in the library engine_harness. The tests use it.
add_library(engine_harness STATIC FileWatcher.cpp)
target_include_directories(engine_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_executable(minengine main.cpp Harness.cpp
    Actor.cpp Util.cpp)
target_link_libraries(minengine PRIVATE spp_backend engine_harness)
set_target_properties(minengine PROPERTIES ENABLE_EXPORTS ON)
