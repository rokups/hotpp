# The translator library sppc_lib. It needs libclang (C API) from the system.
# The tool sppc (src/tools) uses it. The root file sets CLANG_RESOURCE_DIR.
find_library(CLANG_C_LIBRARY NAMES clang REQUIRED)
find_path(CLANG_C_INCLUDE_DIR clang-c/Index.h REQUIRED)
add_library(sppc_lib STATIC sppc_lib.cpp emit.cpp lower.cpp types.cpp check.cpp diag_rewrite.cpp
    parser/script_grammar.cpp
    parser/expression_grammar.cpp parser/statement_grammar.cpp)
target_include_directories(sppc_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(sppc_lib PRIVATE ${CLANG_C_INCLUDE_DIR})
target_link_libraries(sppc_lib PUBLIC ${CLANG_C_LIBRARY})
target_link_libraries(sppc_lib PRIVATE foonathan::lexy::core)
target_compile_definitions(sppc_lib PRIVATE SPP_RESOURCE_DIR="${CLANG_RESOURCE_DIR}")
