// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The program of the sample. The C++ of the script is part of this binary. The program has no
// loader and no compiler at run time.
#include "Creature.h"
#include "hot/aot.h"

#include <cstdio>
#include <exception>
#include <memory>

int main()
{
    try
    {
        // The registry makes the script classes known. hot::aot_table comes from the library
        // sample_aot_scripts.
        hot::AotRegistry registry(hot::aot_table);
        std::unique_ptr<Creature> wolf(registry.create_as<Creature>("Wolf"));
        if (!wolf)
        {
            std::fprintf(stderr, "sample_aot: the class Wolf is not available\n");
            return 1;
        }
        for (int frame = 0; frame < 3; ++frame)
            wolf->Tick(1.0f / 60.0f);
        std::fflush(stdout);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "sample_aot: %s\n", e.what());
        return 1;
    }
    return 0;
}
