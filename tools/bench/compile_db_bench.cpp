// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Reads a compile_commands.json with ClangFilesBackend::read_compile_commands and prints the time
// split. The backend prints the split to stderr when HOT_TIMING is set.
//   HOT_TIMING=1 compile_db_bench DB_DIR GENERATED_DIR [REPS] [N_OWN_UNITS]
#include "hot/clang_files_backend.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: compile_db_bench DB_DIR GENERATED_DIR [REPS]\n");
        return 2;
    }
    hot::ClangFilesOptions o;
    o.compile_commands_dir = argv[1];
    o.generated_dir = argv[2];
    o.work_dir = "/tmp/compile_db_bench_work";
    hot::ClangFilesBackend backend(std::move(o));
    // N own units (default 0): a fake generated file for each, then one first flush.
    if (argc > 4)
    {
        const size_t n = std::strtoul(argv[4], nullptr, 10);
        std::filesystem::create_directories(argv[2]);
        for (size_t i = 0; i < n; ++i)
            std::ofstream(std::string(argv[2]) + "/unit" + std::to_string(i) + ".cpp") << "int x;\n";
        const auto t0 = std::chrono::steady_clock::now();
        backend.flush_for_bench(n, argv[2]);
        std::printf("first flush total %.1f ms\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return 0;
    }
    const int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    for (int i = 0; i < reps; ++i)
    {
        std::string text;
        const auto t0 = std::chrono::steady_clock::now();
        const size_t own = backend.read_compile_db_for_bench(text);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("total %.1f ms, own entries kept %zu, bytes %zu\n", ms, own, text.size());
    }
}
