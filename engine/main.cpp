// minengine: the harness of the hot-reload prototype (design section 8.1).
// Usage: minengine <library.so>
// The program loads the library, makes one Player and ticks the world 3 times.
// The tests are GoogleTest cases in tests/.
#include <cstdio>
#include <string>

#include "Actor.h"
#include "script_runtime.h"

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: minengine <library.so>\n");
        return 2;
    }
    script::Loader loader;  // declared first: it is destroyed last
    World world;            // the world deletes the actors, the library must still be loaded

    std::string error;
    if (!loader.load(argv[1], error)) {
        std::fprintf(stderr, "load failed: %s\n", error.c_str());
        return 1;
    }
    const script::ClassEntry* entry = loader.find("Player");
    if (!entry) {
        std::fprintf(stderr, "class Player is not registered\n");
        return 1;
    }
    world.Add(static_cast<Actor*>(entry->create().native));
    for (int i = 0; i < 3; ++i) world.Tick(1.0f / 60.0f);
    return 0;
}
