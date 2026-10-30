// The program of the sample. It loads generation 1 of wolf, ticks, reloads generation 2 and
// ticks the same object again. The object keeps its field values.
// The host needs a C++ compiler at run time.
// The build makes two programs from this file. The definitions SAMPLE_SCRIPT_DIR and
// SAMPLE_SCRIPT_EXT select the script variant (.hot or .cpp).
#include "Creature.h"
#include "hot/backend.h"
#include "hot/runtime/loader.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
bool read_file(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}
} // namespace

int main()
{
    const std::string path_v1 = std::string(SAMPLE_SCRIPT_DIR) + "/wolf_v1" + SAMPLE_SCRIPT_EXT;
    const std::string path_v2 = std::string(SAMPLE_SCRIPT_DIR) + "/wolf_v2" + SAMPLE_SCRIPT_EXT;
    std::string text_v1;
    std::string text_v2;
    if (!read_file(path_v1, text_v1) || !read_file(path_v2, text_v2))
    {
        std::fprintf(stderr, "sample_hot_reload: cannot read the scripts in %s\n", SAMPLE_SCRIPT_DIR);
        return 1;
    }

    // The backend runs the compiler. The folder of Creature.h is an include dir of the compile.
    hot::DefaultBackendOptions options;
    options.include_dirs = {SAMPLE_DIR};
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend(options);

    // The loader is first, so it is destroyed last. The script object dies before it.
    hot::Loader loader;

    // Generation 1. The first module goes through load().
    hot::LoadResult first = backend->load({path_v1, text_v1, {}}, 1);
    if (!first)
    {
        std::fprintf(stderr, "sample_hot_reload: compile failed: %s\n", first.error.c_str());
        return 1;
    }
    const hot::ReloadResult loaded = loader.load(std::move(*first.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_hot_reload: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const hot::ClassEntry* entry = loader.find("Wolf");
    if (!entry)
    {
        std::fprintf(stderr, "sample_hot_reload: the class Wolf is not available\n");
        return 1;
    }
    std::unique_ptr<Creature> wolf(static_cast<Creature*>(entry->create().native));
    for (int frame = 0; frame < 2; ++frame)
        wolf->Tick(1.0f / 60.0f);

    // Phase 1, validate. Compile generation 2 and check it. The live objects do not change.
    // On an error the old generation goes on.
    const hot::ReloadResult requested = loader.request_reload(backend->load({path_v2, text_v2, {}}, 2));
    if (!requested)
    {
        std::fprintf(stderr, "sample_hot_reload: reload failed: %s\n", requested.error.c_str());
        return 1;
    }

    // Phase 2, commit. The new code goes live. The host calls this at a safe point, when no
    // script code runs. The live objects keep their fields and get the new fields.
    const hot::ReloadResult committed = loader.commit_pending();
    if (!committed)
    {
        std::fprintf(stderr, "sample_hot_reload: commit failed: %s\n", committed.error.c_str());
        return 1;
    }

    // Phase 3, unload. The frame boundary unloads the code of generation 1 when it is not in use.
    loader.frame_boundary();

    // The same pointer now runs the code of generation 2.
    for (int frame = 0; frame < 2; ++frame)
        wolf->Tick(1.0f / 60.0f);
    std::fflush(stdout);
    return 0;
}
