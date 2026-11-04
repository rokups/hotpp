// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The program of the lldb session test. It makes a script object and calls Tick.
// The test sets a breakpoint in target.hot, and lldb prints the values there.
#include "Npc.h"
#include "hot/aot.h"

#include <cstdio>
#include <stdexcept>

int main()
{
    try
    {
        hot::AotRegistry registry(hot::aot_table);
        Npc* hero = registry.create_as<Npc>("Hero");
        hero->Tick();
        std::puts("ticked");
        delete hero;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "lldb_target: %s\n", e.what());
        return 1;
    }
    return 0;
}
