// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A sample runner with an engine: sample_runner_game [-I dir]... [--backend=files|jit]
//                                                    [--frames=N] file.hot
// The engine (Engine.h) owns the loop. The script "main" calls run_engine.
// Each frame the hook calls Program::poll. An edit of the script reloads at the frame boundary.
// When a full restart is ready, the launcher calls Options::request_quit.
// Then the loop returns. After that, run_main restarts "main".
#include "Engine.h"

#include "hot/launcher.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

// The statics of the engine. The script modules find them in this executable.
bool Engine::quit = false;
int Engine::max_frames = 60;
std::function<void()> Engine::frame_hook;

namespace
{
const char* status_name(hot::launcher::PollStatus status)
{
    switch (status)
    {
    case hot::launcher::PollStatus::Idle: return "idle";
    case hot::launcher::PollStatus::Building: return "building";
    case hot::launcher::PollStatus::Applied: return "applied";
    case hot::launcher::PollStatus::Failed: return "failed";
    case hot::launcher::PollStatus::RestartPending: return "restart pending";
    }
    return "?";
}
} // namespace

int main(int argc, char** argv)
{
    hot::launcher::Options options;
    std::string script;
    unsigned frame_ms = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-I" && i + 1 < argc)
            options.include_dirs.push_back(argv[++i]);
        else if (a.rfind("-I", 0) == 0 && a.size() > 2)
            options.include_dirs.push_back(a.substr(2));
        else if (a.rfind("--backend=", 0) == 0)
            options.backend = a.substr(10);
        else if (a.rfind("--frames=", 0) == 0)
            Engine::max_frames = std::atoi(a.c_str() + 9);
        else if (a.rfind("--frame-ms=", 0) == 0)
            frame_ms = static_cast<unsigned>(std::atoi(a.c_str() + 11));
        else
            script = a;
    }
    if (script.empty())
    {
        std::fprintf(stderr,
                     "usage: sample_runner_game [-I dir]... [--backend=files|jit] [--frames=N] [--frame-ms=N] file.hot\n");
        return 64;
    }
    // The native header of the engine is next to the entry script. Add its folder to the search path.
    const std::filesystem::path script_dir = std::filesystem::path(script).parent_path();
    options.include_dirs.push_back(script_dir.empty() ? "." : script_dir.string());
    options.request_quit = request_quit;
    std::string error;
    const auto program = hot::launcher::load(options, script, error);
    if (!program)
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    hot::launcher::PollStatus last = hot::launcher::PollStatus::Idle;
    Engine::frame_hook = [&]
    {
        const hot::launcher::PollStatus status = program->poll();
        if (status != last)
            std::fprintf(stderr, "[runner] poll: %s (generation %u)\n", status_name(status),
                         static_cast<unsigned>(program->generation()));
        last = status;
        if (frame_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(frame_ms));
    };
    const int result = hot::launcher::run_main(*program, {});
    Engine::frame_hook = nullptr;
    return result;
}
