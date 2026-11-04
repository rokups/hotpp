// Generation 1 of a .cpp script class with operator members.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

#include <cstdio>

class Vec : public Actor
{
public:
    Vec() : Actor("Vec") {}

    Vec& operator+=(int v)
    {
        n += v;
        return *this;
    }
    bool operator==(const Vec& o) const { return n == o.n; }
    bool operator!=(const Vec& o) const { return n != o.n; }
    bool operator<(const Vec& o) const { return n < o.n; }
    int operator()(int k) const { return n + k; }
    int operator[](int i) const { return n * 10 + i; }
    int operator-() const { return -n; }
    int operator-(const Vec& o) const { return n - o.n; }

    int n = 0;
};

// The module variables keep the pointers to the objects across a reload.
Vec* a = nullptr;
Vec* b = nullptr;

class Driver : public Actor
{
public:
    Driver() : Actor("Driver") {}

    void Tick(float dt) override
    {
        (void)dt;
        if (!a)
        {
            a = new Vec();
            b = new Vec();
        }
        *a += 2;
        *b += 5;
        auto f = [&] { *a += 1; };
        f();
        std::printf("[v1] %d %d %d %d %d %d %d\n", (*a)(3), (*a)[1], int(*a == *b), int(*a != *b), int(*a < *b), -*a,
            *b - *a);
        std::fflush(stdout);
    }
};
