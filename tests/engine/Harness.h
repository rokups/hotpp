// The harness of the test host (design 8.1): one script file, a world, a file watcher
// and a command channel. The class has no I/O of its own except the file and the log.
// The caller feeds it the command lines. The tests in tests/integration/reload use it.
// It is not a model for a user project: copy from samples/ (for example samples/hot-reload).
//
// Commands: "reload" (compile the file now), "stats" (the script_stats text), "quit".
#pragma once
#include "Actor.h"
#include "FileWatcher.h"
#include "spp/backend.h"
#include "spp/runtime.h"

#include <memory>
#include <string>

namespace harness
{

enum class Command
{
    Empty,
    Reload,
    Stats,
    Quit,
    Unknown
};
Command parse_command(const std::string& line);

class Harness
{
public:
    // "script" is a .hot file. "class_name" is the class of the first object.
    Harness(std::string script, std::string class_name = "Player");

    // Compiles the file as generation 1, loads it and adds one object to the world.
    bool start(std::string& error);

    // Compiles the file again and validates it. The commit happens at the end of
    // the next frame. A failure keeps the old generation. When hot reload cannot apply the
    // change (FullReloadRequired), the harness does a full reload at once and the result is
    // Applied. A full reload restarts the generation at 1 and makes the actors again from
    // their saved position and name.
    spp::ReloadResult reload();

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
    Actor* make_actor(); // a new script object of class_name_, or null
    spp::ReloadResult full_reload(const spp::ScriptSource& source, const spp::ReloadResult& needed);
    std::string script_, class_name_;
    std::unique_ptr<spp::Backend> backend_;
    spp::Loader loader_; // before the world: it is destroyed after the world
    World world_;
    FileWatcher watcher_;
    uint32_t next_generation_ = 1;
    unsigned reloads_ = 0;
    std::string saved_; // the actors of a full reload that failed after the reset
    bool restore_pending_ = false; // true: the Loader has no script and "saved_" holds the actors
    bool quit_ = false;
};

} // namespace harness
