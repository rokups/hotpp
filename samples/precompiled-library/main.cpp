// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The program of the sample. It loads a script library that the build made. The program has
// no backend and no compiler at run time. argv[1] is the path of the library.
#include "Creature.h"
#include "hot/module.h"
#include "hot/runtime/loader.h"

#include <cstdio>
#include <memory>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: sample_precompiled <library.so>\n");
        return 2;
    }
    // The loader is first, so it is destroyed last. The script object dies before it.
    hot::Loader loader;
    hot::LoadResult module = hot::module_from_library(argv[1]);
    if (!module)
    {
        std::fprintf(stderr, "sample_precompiled: %s\n", module.error.c_str());
        return 1;
    }
    const hot::ReloadResult loaded = loader.load(std::move(*module.module));
    if (!loaded)
    {
        std::fprintf(stderr, "sample_precompiled: load failed: %s\n", loaded.error.c_str());
        return 1;
    }
    const hot::ClassEntry* entry = loader.find("Wolf");
    if (!entry)
    {
        std::fprintf(stderr, "sample_precompiled: the class Wolf is not available\n");
        return 1;
    }
    std::unique_ptr<Creature> wolf(static_cast<Creature*>(entry->create().native));
    for (int frame = 0; frame < 3; ++frame)
        wolf->Tick(1.0f / 60.0f);
    std::fflush(stdout);
    return 0;
}
