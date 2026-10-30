// The program of the sample. It builds ClangFilesBackend itself, compiles wolf.spp with it and
// calls Tick. The host needs a C++ compiler at run time.
#include "Creature.h"
#include "spp/clang_files_backend.h"
#include "spp/runtime/loader.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

int main()
{
    const std::string path = std::string(SAMPLE_DIR) + "/wolf.spp";
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "sample_clang_files: cannot read %s\n", path.c_str());
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();

    // The program sets the compiler, the include dirs, the flags and the work folder.
    spp::ClangFilesOptions options;
    options.compiler = SAMPLE_CXX;
    options.include_dirs = {SAMPLE_SPP_INCLUDE_DIR, SAMPLE_DIR};
    // The flags of the host build, for example a sanitizer. The modules must use the same ones.
    std::istringstream flags(SAMPLE_CXXFLAGS);
    for (std::string flag; flags >> flag;)
        options.extra_flags.push_back(flag);
    options.resource_dir = SAMPLE_RESOURCE_DIR;
    options.work_dir = SAMPLE_WORK_DIR;
    options.remove_work_dir_at_exit = true;
    spp::ClangFilesBackend backend(std::move(options));

    // The loader is after the backend, so it is destroyed first. The script object dies before it.
    spp::Loader loader;
    const spp::ScriptSource source{path, text.str(), {}};
    spp::LoadResult built = backend.load(source, 1);
    if (!built)
    {
        std::fprintf(stderr, "sample_clang_files: compile failed: %s\n", built.error.c_str());
        return 1;
    }
    const spp::ReloadResult loaded = loader.load(std::move(*built.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_clang_files: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const spp::ClassEntry* entry = loader.find("Wolf");
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
