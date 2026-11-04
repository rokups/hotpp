// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The host class of the lldb session test. The script class Hero derives from it.
#pragma once

class Npc
{
public:
    virtual ~Npc() = default;
    virtual void Tick() {}
};
