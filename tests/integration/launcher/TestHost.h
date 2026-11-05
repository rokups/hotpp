// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native host class that simulates an engine loop. A script derives from it and overrides Step.
// The script creates the object in "main" and calls Run, a blocking native loop.
// The test copies this header into its temp dir, so the script can write: using "TestHost.h";
#pragma once
#include "hot/runtime/export.h"

#include <functional>

// HOT_EXPORT: the script module derives from the class and uses the statics of the host.
class HOT_EXPORT TestHost
{
public:
    virtual ~TestHost() = default;

    // A script class overrides it. The result adds to the total of Run.
    virtual int Step() { return 0; }

    // The loop. It sets "running" to true when it starts, so a restarted "main" runs again after
    // a quit. The frame starts at 1. Each iteration calls on_frame(frame), then Step.
    // The loop ends when "running" is false after Step. It returns the sum of the Step results.
    int Run()
    {
        running = true;
        int total = 0;
        for (int frame = 1; running; ++frame)
        {
            if (on_frame)
                on_frame(frame);
            total += Step();
        }
        return total;
    }

    // The test sets both. They live in the host executable (the test .cpp defines them).
    static std::function<void(int frame)> on_frame;
    static bool running;
};
