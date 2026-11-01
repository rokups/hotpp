# Copyright (c) 2026 the Hot++ project.
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

# ------------------------------------------------------------ Language server (S14b)
# hot-ls is a stdio LSP proxy. It translates a .hot document in memory and gives the generated
# C++ to a stock clangd child process. It maps the positions with the source map. The library
# hot_ls_lib holds the server, so the tests run it in the test process over pipes.
add_library(hot_ls_lib STATIC jsonrpc.cpp clangd_process.cpp server.cpp
    semantic.cpp symbol_index.cpp hosts.cpp)
target_include_directories(hot_ls_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/src/translator)
target_link_libraries(hot_ls_lib PUBLIC hot_clang_backend nlohmann_json::nlohmann_json Threads::Threads)
add_executable(hot_ls main.cpp)
target_link_libraries(hot_ls PRIVATE hot_ls_lib)
set_target_properties(hot_ls PROPERTIES OUTPUT_NAME hot-ls)
