// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native class with an intrusive reference count. Scripts hold it through
// hot::Intrusive. The definitions are in Object.cpp, so the executable
// has the only copy of the counter "live".
#pragma once

struct Object
{
    int refs = 0;
    static int live; // The number of Object instances that exist.

    Object();
    virtual ~Object();
    void AddRef();
    void Release();
};

struct Item : Object
{
    int value = 0;
    explicit Item(int v)
        : value(v)
    {
    }
};
