# The translator library hotc_lib. It needs libclang (C API) from the system.
# The tool hotc (src/tools) uses it. The root file sets CLANG_RESOURCE_DIR.
find_library(CLANG_C_LIBRARY NAMES clang REQUIRED)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
add_library(hotc_lib STATIC hotc_lib.cpp emit.cpp lower.cpp types.cpp check.cpp diag_rewrite.cpp
    parser/script_grammar.cpp parser/cpp_frontend.cpp
    parser/expression_grammar.cpp parser/statement_grammar.cpp)
target_include_directories(hotc_lib PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(hotc_lib PRIVATE ${CLANG_C_INCLUDE_DIR})
# The export holds no path of libclang. HotConfig.cmake finds the library for the consumer.
target_link_libraries(hotc_lib PUBLIC $<BUILD_INTERFACE:${CLANG_C_LIBRARY}>)
target_link_libraries(hotc_lib PRIVATE $<BUILD_INTERFACE:foonathan::lexy::core>)
# The translator needs C++20. The host can use C++17. The parse of the native headers uses the
# standard of the host (HOT_CXX_STANDARD, include/hot/config.h), not the standard of this target.
target_compile_features(hotc_lib PRIVATE cxx_std_20)
hot_effective_cxx_standard(HOT_HOST_CXX_STANDARD hot_runtime_core)
target_compile_definitions(hotc_lib PRIVATE HOT_CXX_STANDARD=${HOT_HOST_CXX_STANDARD})
if(HOT_PORTABLE)
    # A portable build holds no build path. The empty value means: the caller or libclang decides.
    target_compile_definitions(hotc_lib PRIVATE HOT_RESOURCE_DIR="")
else()
    target_compile_definitions(hotc_lib PRIVATE HOT_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
endif()
add_library(hot::hotc_lib ALIAS hotc_lib)
