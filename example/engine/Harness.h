// The harness of minengine (design 8.1): one script file, a world, a file watcher
// and a command channel. The class has no I/O of its own except the file and the log.
// main.cpp feeds it the lines of stdin.
//
// Commands: "reload" (compile the file now), "stats" (the script_stats text), "quit".
#pragma once
#include <memory>
#include <string>

#include "Actor.h"
#include "FileWatcher.h"
#include "spp/backend.h"
#include "spp/runtime.h"

namespace harness {

enum class Command { Empty, Reload, Stats, Quit, Unknown };
Command parse_command(const std::string& line);

class Harness {
public:
    // "script" is a .spp file. "class_name" is the class of the first object.
    Harness(std::string script, std::string class_name = "Player");

    // Compiles the file as generation 1, loads it and adds one object to the world.
    bool start(std::string& error);

    // Compiles the file again and validates it. The commit happens at the end of
    // the next frame. A failure keeps the old generation.
    bool reload(std::string& error);

    // Runs one command line. The result is the text for the user.
    std::string command(const std::string& line);

    // One frame: reload when the file changed (waits up to "wait_ms" for a change),
    // then tick the world. The frame boundary hook commits a pending reload.
    void frame(float dt, int wait_ms = 0);

    bool quit_requested() const { return quit_; }
    spp::Loader& loader() { return loader_; }
    World& world() { return world_; }
    unsigned reloads_started() const { return reloads_; }

private:
    std::string script_, class_name_;
    std::unique_ptr<spp::Backend> backend_;
    spp::Loader loader_;  // before the world: it is destroyed after the world
    World world_;
    FileWatcher watcher_;
    uint32_t next_generation_ = 1;
    unsigned reloads_ = 0;
    bool quit_ = false;
};

}  // namespace harness
