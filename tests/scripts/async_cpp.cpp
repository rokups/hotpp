// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A ".cpp" script with plain hot::Task coroutines (C++20). The ".cpp" frontend has no "async"
// rewriting: the methods use co_await and co_return directly (tests/integration/async).
#include "Actor.h"
#include "hot/runtime/task.h"

class CppAsync : public Actor
{
public:
    CppAsync() : Actor("CppAsync") {}

    hot::Task<int> Half(int x) { co_return x / 2; }

    hot::Task<int> Quarter(int x)
    {
        int h = co_await Half(x);
        calls += 1;
        co_return co_await Half(h);
    }

    int calls = 0;
};
