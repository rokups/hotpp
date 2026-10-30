// The program of the sample. It builds JitBackend itself, compiles the script wolf in memory with it and
// calls Tick. The host needs the Clang library, but no external compiler process.
// The build makes two programs from this file. The definitions SAMPLE_SCRIPT_DIR and
// SAMPLE_SCRIPT_EXT select the script variant (.spp or .cpp).
#include "Creature.h"
#include "spp/jit_backend.h"
#include "spp/runtime/loader.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

int main()
{
    const std::string path = std::string(SAMPLE_SCRIPT_DIR) + "/wolf" + SAMPLE_SCRIPT_EXT;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "sample_jit: cannot read %s\n", path.c_str());
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();

    // The program sets the compiler name, the include dirs, the resource dir and the flags.
    spp::JitOptions options;
    options.compiler = SAMPLE_CXX;
    options.include_dirs = {SAMPLE_SPP_INCLUDE_DIR, SAMPLE_DIR};
    options.resource_dir = SAMPLE_RESOURCE_DIR;
    // The flags of the host, for example a sanitizer. The JIT code must use the same ones.
    std::istringstream flags(SAMPLE_CXXFLAGS);
    for (std::string flag; flags >> flag;)
        options.extra_flags.push_back(flag);
    spp::JitBackend backend(std::move(options));

    // The loader is after the backend, so it is destroyed first. The script object dies before it.
    spp::Loader loader;
    const spp::ScriptSource source{path, text.str(), {}};
    spp::LoadResult built = backend.load(source, 1);
    if (!built)
    {
        std::fprintf(stderr, "sample_jit: compile failed: %s\n", built.error.c_str());
        return 1;
    }
    const spp::ReloadResult loaded = loader.load(std::move(*built.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_jit: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const spp::ClassEntry* entry = loader.find("Wolf");
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
