// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A tiny native engine that owns the loop. A script derives from Game and overrides the events.
// The script calls run_engine in "main". The runner (main_game.cpp) sets the hook and the limit.
// The script reads this header with: using "Engine.h";
#pragma once
#include "hot/runtime/export.h"

#include <functional>

// HOT_EXPORT: the script module derives from the class and uses the statics of the engine.
class HOT_EXPORT Game
{
public:
    virtual ~Game() = default;

    virtual void on_start() {}
    virtual void on_frame(int frame) { (void)frame; }
    virtual void on_stop() {}
};

// The state of the engine. The runner defines these statics. The script modules find them there.
struct HOT_EXPORT Engine
{
    static bool quit;                          // true: the loop ends after the current frame
    static int max_frames;                     // the loop ends after this number of frames
    static std::function<void()> frame_hook;   // runs once for each frame, before on_frame
};

// Asks the loop to end. The launcher calls it through Options::request_quit.
inline void request_quit()
{
    Engine::quit = true;
}

// Runs the frames 1, 2, ... until request_quit or Engine::max_frames. Calls on_start before the
// first frame and on_stop after the last frame. The function resets the quit flag at its start.
// So a restarted "main" runs the loop again.
inline void run_loop(Game& game, int max_frames, const std::function<void()>& frame_hook)
{
    Engine::quit = false;
    game.on_start();
    for (int frame = 1; !Engine::quit && frame <= max_frames; ++frame)
    {
        if (frame_hook)
            frame_hook();
        game.on_frame(frame);
    }
    game.on_stop();
}

// The entry point for scripts: the loop with the limit and the hook of the runner.
inline void run_engine(Game& game)
{
    run_loop(game, Engine::max_frames, Engine::frame_hook);
}
