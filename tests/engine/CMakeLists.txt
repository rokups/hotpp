# The tests link this library for the file watcher.
add_library(engine_harness STATIC FileWatcher.cpp)
target_include_directories(engine_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
