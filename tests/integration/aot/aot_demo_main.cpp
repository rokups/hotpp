// aot_demo: the AOT mode (design M7) in a program. The C++ of the script
// tests/scripts/aot_demo.hot is part of this binary. The program has no loader, no file
// watcher and no compiler. It prints the state of the objects after each frame.
// Usage: aot_demo
#include "demo_run.h"
#include "spp/aot.h"

#include <cstdio>
#include <exception>

int main()
{
    try
    {
        spp::AotRegistry registry(spp::aot_table);
        const std::string log = aot_demo::run([&](const char* name) { return registry.create(name); });
        std::fputs(log.c_str(), stdout);
        std::fflush(stdout);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "aot_demo: %s\n", e.what());
        return 1;
    }
    return 0;
}
