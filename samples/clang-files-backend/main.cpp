// The program of the sample. It builds ClangFilesBackend itself, compiles the script wolf with it and
// calls Tick. The host needs a C++ compiler at run time.
// The build makes two programs from this file. The arguments select the script variant:
//   sample_clang_files [script-dir [extension]]
// The folder defaults to the definition HOT_SCRIPT_DIR. The extension (.hot or .cpp) defaults to
// the definition HOT_SAMPLE_EXT, else .hot.
#include "Creature.h"
#include "hot/clang_files_backend.h"
#include "hot/paths.h"
#include "hot/runtime/loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
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
        std::fprintf(stderr, "usage: sample_clang_files <script-dir> [extension]\n");
        return 2;
    }
    const std::string ext = argc > 2 ? argv[2] : script_ext();
    const std::string path = dir + "/wolf" + ext;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "sample_clang_files: cannot read %s\n", path.c_str());
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();

    // The program sets the compiler, the include dirs, the flags and the work folder.
    // hot::default_paths() gives the defaults. A portable build finds them at run time.
    const hot::DefaultPaths paths = hot::default_paths();
    hot::ClangFilesOptions options;
    options.compiler = paths.compiler;
    options.include_dirs = paths.include_dirs;
    // The folder of Creature.h is the parent of the script folder.
    options.include_dirs.push_back(std::filesystem::path(dir).parent_path().string());
    // The flags of the host build, for example a sanitizer. The modules must use the same ones.
    options.extra_flags = paths.cxxflags;
    options.resource_dir = paths.resource_dir;
    // One work folder for each process.
    options.work_dir = paths.work_dir + "/sample_clang_files_p" + std::to_string(getpid());
    options.remove_work_dir_at_exit = true;
    hot::ClangFilesBackend backend(std::move(options));

    // The loader is after the backend, so it is destroyed first. The script object dies before it.
    hot::Loader loader;
    const hot::ScriptSource source{path, text.str(), {}};
    hot::LoadResult built = backend.load(source, 1);
    if (!built)
    {
        std::fprintf(stderr, "sample_clang_files: compile failed: %s\n", built.error.c_str());
        return 1;
    }
    const hot::ReloadResult loaded = loader.load(std::move(*built.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_clang_files: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const hot::ClassEntry* entry = loader.find("Wolf");
    if (!entry)
    {
        std::fprintf(stderr, "sample_clang_files: the class Wolf is not available\n");
        return 1;
    }
    std::unique_ptr<Creature> wolf(static_cast<Creature*>(entry->create().native));
    for (int frame = 0; frame < 3; ++frame)
        wolf->Tick(1.0f / 60.0f);
    std::fflush(stdout);
    return 0;
}
