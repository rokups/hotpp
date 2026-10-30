# sppc is the S++ tool. It translates a .hot file (--emit-cpp) or compiles it ahead
# of time into a .so. The output loads in every build.
add_executable(sppc sppc.cpp)
target_link_libraries(sppc PRIVATE spp_clang_backend)
target_compile_features(sppc PRIVATE cxx_std_20)
