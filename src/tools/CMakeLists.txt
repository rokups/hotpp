# hotc is the Hot++ tool. It translates a .hot file (--emit-cpp) or compiles it ahead
# of time into a .so. The output loads in every build.
add_executable(hotc hotc.cpp)
target_link_libraries(hotc PRIVATE hot_clang_backend)
target_compile_features(hotc PRIVATE cxx_std_20)
