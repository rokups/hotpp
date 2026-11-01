// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The native base class of the benchmark scripts. Each script class derives from BenchBase.
// The benchmark also has a native C++ twin of each script class. The twin is the reference.
#pragma once
#include <cstdint>

class BenchBase
{
public:
    virtual ~BenchBase() = default;

    // The body of a case. The host calls it once with n loop iterations.
    // The case "empty" has an empty body. The host calls it n times with n = 0.
    virtual int64_t Run(int n);

    // A virtual hook. The method DriveHook calls it n times (native to script call).
    virtual int Hook(int x);
    int64_t DriveHook(int n);

    // A native function that a script calls (script to native call). It lives in another
    // translation unit, so the compiler cannot inline it.
    int NativeAdd(int a, int b);
};
