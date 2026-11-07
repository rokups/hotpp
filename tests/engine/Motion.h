// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native class with "T&" getters. A script binds a "ref" to the result of a getter.
#pragma once

struct Motion
{
    float x = 0;
    int frame = 0;
};

class Animator
{
public:
    Animator() = default;
    explicit Animator(int f) { motion_.frame = f; }
    Motion& GetMotion() { return motion_; }
    Motion& MotionAt(int i) { return motions_[i]; }
    static Motion& Shared()
    {
        static Motion m;
        return m;
    }

private:
    Motion motion_;
    Motion motions_[2];
};
