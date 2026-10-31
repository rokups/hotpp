# hotc is the Hot++ tool. It translates a .hot file (--emit-cpp) or compiles it ahead
# of time into a .so. The output loads in every build.
add_executable(hotc hotc.cpp)
add_executable(hot::hotc ALIAS hotc)
target_link_libraries(hotc PRIVATE hot_clang_backend)
