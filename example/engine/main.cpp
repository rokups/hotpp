// minengine: the harness of the hot-reload prototype (design section 8.1).
// Usage: minengine <library.so>
//   Loads the library, makes one Player and ticks the world 3 times.
// Usage: minengine <script.spp> [ClassName]
//   Compiles the script, makes one object (default class Player) and ticks at 60 Hz.
//   The program reloads the script when the file changes. Commands on stdin:
//   reload, stats, quit. The program also ends when stdin ends.
// The tests are GoogleTest cases in tests/.
#include <poll.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <string>

#include "Actor.h"
#include "Harness.h"
#include "spp/runtime.h"

static bool ends_with(const std::string& s, const char* suffix)
{
    const std::string x = suffix;
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

static int run_script(const std::string& path, const std::string& class_name)
{
    harness::Harness h(path, class_name);
    std::string error;
    if (!h.start(error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    std::string line_buffer;
    bool eof = false;
    while (!h.quit_requested() && !eof) {
        // The frame waits on the file watcher for one frame time. Then stdin is read.
        h.frame(1.0f / 60.0f, 16);
        pollfd in{STDIN_FILENO, POLLIN, 0};
        while (poll(&in, 1, 0) > 0) {
            char c;
            const ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) {
                eof = true;
                break;
            }
            if (c != '\n') {
                line_buffer += c;
                continue;
            }
            std::fputs(h.command(line_buffer).c_str(), stdout);
            std::fflush(stdout);
            line_buffer.clear();
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 2 && ends_with(argv[1], ".spp"))
        return run_script(argv[1], argc >= 3 ? argv[2] : "Player");
    if (argc != 2) {
        std::fprintf(stderr, "usage: minengine <library.so> | <script.spp> [ClassName]\n");
        return 2;
    }
    spp::Loader loader;  // declared first: it is destroyed last
    World world;            // the world deletes the actors, the library must still be loaded

    std::string error;
    spp::LoadResult module = spp::module_from_library(argv[1]);
    if (!module || !loader.load(std::move(*module.module), error)) {
        std::fprintf(stderr, "load failed: %s\n", module ? error.c_str() : module.error.c_str());
        return 1;
    }
    const spp::ClassEntry* entry = loader.find("Player");
    if (!entry) {
        std::fprintf(stderr, "class Player is not registered\n");
        return 1;
    }
    world.Add(static_cast<Actor*>(entry->create().native));
    for (int i = 0; i < 3; ++i) world.Tick(1.0f / 60.0f);
    return 0;
}
