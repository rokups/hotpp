// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The program of the sample. It builds JitBackend itself, compiles the script wolf in memory with it and
// calls Tick. The host needs the Clang library, but no external compiler process.
// The build makes two programs from this file. The arguments select the script variant:
//   sample_jit [script-dir [extension]]
// The folder defaults to the definition HOT_SCRIPT_DIR. The extension (.hot or .cpp) defaults to
// the definition HOT_SAMPLE_EXT, else .hot.
#include "Creature.h"
#include "hot/jit_backend.h"
#include "hot/paths.h"
#include "hot/runtime/loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace
{
// The script extension of this variant: the build definition HOT_SAMPLE_EXT, else ".hot".
std::string script_ext()
{
#ifdef HOT_SAMPLE_EXT
    return HOT_SAMPLE_EXT;
#else
    return ".hot";
#endif
}

// The folder of the scripts: argv[1], else the build definition HOT_SCRIPT_DIR. Empty if none.
std::string script_dir(int argc, char** argv)
{
    if (argc > 1)
        return argv[1];
#ifdef HOT_SCRIPT_DIR
    return HOT_SCRIPT_DIR;
#else
    return {};
#endif
}
} // namespace

int main(int argc, char** argv)
{
    const std::string dir = script_dir(argc, argv);
    if (dir.empty())
    {
        std::fprintf(stderr, "usage: sample_jit <script-dir> [extension]\n");
        return 2;
    }
    const std::string ext = argc > 2 ? argv[2] : script_ext();
    const std::string path = dir + "/wolf" + ext;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "sample_jit: cannot read %s\n", path.c_str());
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();

    // The program sets the compiler name, the include dirs, the resource dir and the flags.
    // hot::default_paths() gives the defaults. A portable build finds them at run time.
    const hot::DefaultPaths paths = hot::default_paths();
    hot::JitOptions options;
    options.compiler = paths.compiler;
    options.include_dirs = paths.include_dirs;
    // The folder of Creature.h is the parent of the script folder.
    options.include_dirs.push_back(std::filesystem::path(dir).parent_path().string());
    options.resource_dir = paths.resource_dir;
    // The flags of the host, for example a sanitizer. The JIT code must use the same ones.
    options.extra_flags = paths.cxxflags;
    hot::JitBackend backend(std::move(options));

    // The loader is after the backend, so it is destroyed first. The script object dies before it.
    hot::Loader loader;
    const hot::ScriptSource source{path, text.str(), {}};
    hot::LoadResult built = backend.load(source, 1);
    if (!built)
    {
        std::fprintf(stderr, "sample_jit: compile failed: %s\n", built.error.c_str());
        return 1;
    }
    const hot::ReloadResult loaded = loader.load(std::move(*built.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_jit: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const hot::ClassEntry* entry = loader.find("Wolf");
    if (!entry)
    {
        std::fprintf(stderr, "sample_jit: the class Wolf is not available\n");
        return 1;
    }
    std::unique_ptr<Creature> wolf(static_cast<Creature*>(entry->create().native));
    for (int frame = 0; frame < 3; ++frame)
        wolf->Tick(1.0f / 60.0f);
    std::fflush(stdout);
    return 0;
}
