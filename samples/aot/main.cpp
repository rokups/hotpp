// The program of the sample. The C++ of wolf.spp is part of this binary. The program has no
// loader and no compiler at run time.
#include "Creature.h"
#include "spp/aot.h"

#include <cstdio>
#include <exception>
#include <memory>

int main()
{
    try
    {
        // The registry makes the script classes known. spp::aot_table comes from the library
        // sample_aot_scripts.
        spp::AotRegistry registry(spp::aot_table);
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
