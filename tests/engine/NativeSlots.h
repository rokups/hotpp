// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native class with a member name that holds a part of a generated name ("max_slots"). The LS
// tests use it (HOT-95).
#pragma once
#include "hot/runtime/export.h"

class HOT_EXPORT NativeSlots
{
public:
    NativeSlots() = default;
    virtual ~NativeSlots() = default;
    int max_slots = 3;
};
