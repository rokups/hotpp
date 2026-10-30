# ------------------------------------------------------------ Language server (S14b)
# spp-ls is a stdio LSP proxy. It translates a .hot document in memory and gives the generated
# C++ to a stock clangd child process. It maps the positions with the source map. The library
# spp_ls_lib holds the server, so the tests run it in the test process over pipes.
add_library(spp_ls_lib STATIC jsonrpc.cpp clangd_process.cpp server.cpp
    semantic.cpp symbol_index.cpp hosts.cpp)
target_include_directories(spp_ls_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/src/translator)
target_compile_features(spp_ls_lib PUBLIC cxx_std_20)
target_link_libraries(spp_ls_lib PUBLIC spp_clang_backend nlohmann_json::nlohmann_json Threads::Threads)
add_executable(spp_ls main.cpp)
target_link_libraries(spp_ls PRIVATE spp_ls_lib)
set_target_properties(spp_ls PROPERTIES OUTPUT_NAME spp-ls)
