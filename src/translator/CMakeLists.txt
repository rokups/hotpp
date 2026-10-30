# The translator library sppc_lib. It needs libclang (C API) from the system.
# The tool sppc (src/tools) uses it. The root file sets CLANG_RESOURCE_DIR.
find_library(CLANG_C_LIBRARY NAMES clang REQUIRED)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
add_library(sppc_lib STATIC sppc_lib.cpp emit.cpp lower.cpp types.cpp check.cpp diag_rewrite.cpp
    parser/script_grammar.cpp
    parser/expression_grammar.cpp parser/statement_grammar.cpp)
target_include_directories(sppc_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(sppc_lib PRIVATE ${CLANG_C_INCLUDE_DIR})
target_link_libraries(sppc_lib PUBLIC ${CLANG_C_LIBRARY})
target_link_libraries(sppc_lib PRIVATE foonathan::lexy::core)
# The translator needs C++20. The host can use C++17. The parse of the native headers uses the
# standard of the host (SPP_CXX_STANDARD, include/spp/config.h), not the standard of this target.
target_compile_features(sppc_lib PRIVATE cxx_std_20)
spp_effective_cxx_standard(SPP_HOST_CXX_STANDARD spp_runtime_core)
target_compile_definitions(sppc_lib PRIVATE SPP_CXX_STANDARD=${SPP_HOST_CXX_STANDARD})
target_compile_definitions(sppc_lib PRIVATE SPP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
