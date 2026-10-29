// Native helpers for script code. The definitions are in Util.cpp.
// They are not inline and not templates, so the host executable must export them.
// Exception: the inline function pointer helpers at the end of the file (S17).
#pragma once
#include <string>

#include "Actor.h"

// The distance between two points.
float Distance(const Vec3& a, const Vec3& b);

// A text such as "(1.5, 2, 3)".
std::string FormatVec(const Vec3& v);

struct Util {
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
inline int TwiceInt(int x) { return 2 * x; }
inline IntFn PickDoubler() { return &TwiceInt; }
inline int CallIntFn(IntFn f, int x) { return f ? f(x) : 0; }
