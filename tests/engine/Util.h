// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Native helpers for script code. The definitions are in Util.cpp.
// They are not inline and not templates, so the host executable exports them (HOT_EXPORT).
// Exception: the inline function pointer helpers at the end of the file (S17).
#pragma once
#include "Actor.h"

#include <string>

// The distance between two points.
HOT_EXPORT float Distance(const Vec3& a, const Vec3& b);

// Native functions with a pointer parameter. The script passes a value lvalue: "Nudge(v, 1)".
// Peek takes "null" too. It gives -1 for a null pointer.
HOT_EXPORT void Nudge(Vec3* v, float dx);
HOT_EXPORT float Peek(const Vec3* v);

// Native functions with a scalar pointer parameter. "Bump" adds "by" to "*v" (an out-parameter:
// the script passes a local, "Bump(n, 2)"). "SumData" reads "n" floats from "data". A script
// cannot pass one float to it: the translator refuses a "const float*" parameter.
HOT_EXPORT void Bump(int* v, int by);
HOT_EXPORT float SumData(const float* data, int n);

// Like strtof. "*end" gets the pointer behind the number. The script passes a local "char* end".
// "Consumed" gives the number of characters between "s" and "end".
HOT_EXPORT float ParseFloat(const char* s, char** end);
HOT_EXPORT int Consumed(const char* s, const char* end);

// A text such as "(1.5, 2, 3)".
HOT_EXPORT std::string FormatVec(const Vec3& v);

struct HOT_EXPORT Util
{
    // The sum of the three components.
    static float Sum(const Vec3& v);
    // The text "<name>@<x>" for an actor.
    static std::string Describe(const Actor& actor);
    // Counts one call and returns "v". A test reads the count with Calls().
    static int Count(int v);
    // The number of Count calls since the program start.
    static int Calls();
    // Native code between two script frames (design 6.3, S21). It calls the probe function of
    // a test (when one is set) and then a->TakeDamage(amount). So a script that calls it
    // runs script code again: script -> native -> script.
    using ProbeFn = void (*)(Actor*);
    static void Probe(Actor* a, float amount);
    static void SetProbe(ProbeFn fn);
};

// A raw function pointer for a script state field (design 4.9). The inline functions compile
// into each generation module, so the pointer can point into the code of a generation.
using IntFn = int (*)(int);
inline int TwiceInt(int x)
{
    return 2 * x;
}
inline IntFn PickDoubler()
{
    return &TwiceInt;
}
inline int CallIntFn(IntFn f, int x)
{
    return f ? f(x) : 0;
}

// A trivially copyable struct with a function pointer field (S26). It holds code, but the type
// is plain data. The translator marks it, so a script field of this type blocks the unload.
struct IntCallback
{
    IntFn fn;
    int tag;
};
inline IntCallback MakeIntCallback()
{
    return IntCallback{&TwiceInt, 1};
}
inline int CallIntCallback(const IntCallback& c, int x)
{
    return c.fn ? c.fn(x) : 0;
}
