// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "Util.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

float Distance(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Nudge(Vec3* v, float dx)
{
    if (v)
        v->x += dx;
}

float Peek(const Vec3* v)
{
    return v ? v->x : -1.0f;
}

void Bump(int* v, int by)
{
    if (v)
        *v += by;
}

float SumData(const float* data, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; ++i)
        sum += data[i];
    return sum;
}

float ParseFloat(const char* s, char** end)
{
    return std::strtof(s, end);
}

int Consumed(const char* s, const char* end)
{
    return static_cast<int>(end - s);
}

std::string FormatVec(const Vec3& v)
{
    char buf[96];
    std::snprintf(
        buf, sizeof buf, "(%g, %g, %g)", static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z));
    return buf;
}

float Util::Sum(const Vec3& v)
{
    return v.x + v.y + v.z;
}

std::string Util::Describe(const Actor& actor)
{
    return actor.Name + "@" + FormatVec(actor.Position);
}

namespace
{
int g_count_calls = 0;
}

int Util::Count(int v)
{
    ++g_count_calls;
    return v;
}

int Util::Calls()
{
    return g_count_calls;
}

static std::atomic<Util::ProbeFn> g_probe{nullptr};

void Util::SetProbe(ProbeFn fn)
{
    g_probe.store(fn);
}

void Util::Probe(Actor* a, float amount)
{
    if (ProbeFn fn = g_probe.load())
        fn(a);
    a->TakeDamage(amount);
}
