// The program of the sample. It reads the script wolf, compiles it at run time and calls Tick.
// The host needs a C++ compiler at run time.
// The build makes two programs from this file. The definitions SAMPLE_SCRIPT_DIR and
// SAMPLE_SCRIPT_EXT select the script variant (.hot or .cpp).
#include "Creature.h"
#include "spp/backend.h"
#include "spp/runtime/loader.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

int main()
{
    const std::string path = std::string(SAMPLE_SCRIPT_DIR) + "/wolf" + SAMPLE_SCRIPT_EXT;
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "sample_runtime_compile: cannot read %s\n", path.c_str());
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();

    // The backend runs the compiler. The folder of Creature.h is an include dir of the compile.
    spp::DefaultBackendOptions options;
    options.include_dirs = {SAMPLE_DIR};
    std::unique_ptr<spp::Backend> backend = spp::make_default_backend(options);

    // The loader is first, so it is destroyed last. The script object dies before it.
    spp::Loader loader;
    const spp::ScriptSource source{path, text.str(), {}};
    spp::LoadResult built = backend->load(source, 1);
    if (!built)
    {
        std::fprintf(stderr, "sample_runtime_compile: compile failed: %s\n", built.error.c_str());
        return 1;
    }
    const spp::ReloadResult loaded = loader.load(std::move(*built.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_runtime_compile: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const spp::ClassEntry* entry = loader.find("Wolf");
    if (!entry)
    {
        std::fprintf(stderr, "sample_runtime_compile: the class Wolf is not available\n");
        return 1;
    }
    std::unique_ptr<Creature> wolf(static_cast<Creature*>(entry->create().native));
    for (int frame = 0; frame < 3; ++frame)
        wolf->Tick(1.0f / 60.0f);
    std::fflush(stdout);
    return 0;
}
