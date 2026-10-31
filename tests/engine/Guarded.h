// A native interface with a noexcept virtual function (the runtime checks tests).
// The shell override of a noexcept function must not let an exception leave.
#pragma once

class IGuarded
{
public:
    virtual ~IGuarded() = default;
    virtual int Guard(int x) noexcept { return x; }
};

// Native code that calls the noexcept virtual through the interface.
inline int CallGuard(IGuarded& target, int x)
{
    return target.Guard(x);
}
