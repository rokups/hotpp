#include "Util.h"

#include <atomic>
#include <cmath>
#include <cstdio>

float Distance(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
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
