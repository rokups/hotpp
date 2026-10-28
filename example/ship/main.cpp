// ship_demo: the shipping mode (design M7) in a program. The C++ of the script
// example/scripts/ship_demo.spp is part of this binary. The program has no loader, no file
// watcher and no compiler. It prints the state of the objects after each frame.
// Usage: ship_demo
#include <cstdio>
#include <exception>

#include "demo_run.h"
#include "spp/shipped.h"

int main()
{
    try {
        spp::ShippedRegistry registry(spp::shipped_table);
        const std::string log = ship_demo::run([&](const char* name) { return registry.create(name); });
        std::fputs(log.c_str(), stdout);
        std::fflush(stdout);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ship_demo: %s\n", e.what());
        return 1;
    }
    return 0;
}
