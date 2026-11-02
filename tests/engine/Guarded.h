// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A native interface with a noexcept virtual function (the runtime checks tests).
// The shell override of a noexcept function must not let an exception leave.
#pragma once

class IGuarded
{
public:
    virtual ~IGuarded() = default;
    virtual int Guard(int x) noexcept { return x; }
};

// A native interface with a noexcept pure virtual function. An abstract script class leaves it open.
class IStrict
{
public:
    virtual ~IStrict() = default;
    virtual int Seal(int x) noexcept = 0;
};

// Native code that calls the noexcept virtual through the interface.
inline int CallGuard(IGuarded& target, int x)
{
    return target.Guard(x);
}
