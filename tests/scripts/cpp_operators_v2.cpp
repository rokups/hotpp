// Generation 2: the compound operator changes, "operator()" gets a second parameter and
// "operator*" is new.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

#include <cstdio>

class Vec : public Actor
{
public:
    Vec() : Actor("Vec") {}

    Vec& operator+=(int v)
    {
        n += 2 * v;
        return *this;
    }
    bool operator==(const Vec& o) const { return n == o.n; }
    bool operator!=(const Vec& o) const { return n != o.n; }
    bool operator<(const Vec& o) const { return n < o.n; }
    int operator()(int k, int m) const { return n + k * m; }
    int operator[](int i) const { return n * 10 + i; }
    int operator-() const { return -n; }
    int operator-(const Vec& o) const { return n - o.n; }
    int operator*(int k) const { return n * k; }

    int n = 0;
};

// The module variables keep the pointers to the objects across a reload.
Vec* a = nullptr;
Vec* b = nullptr;

class Driver : public Actor
{
public:
    Driver() : Actor("Driver") {}
    // The driver owns the objects of the module variables. Without this, they stay alive after the test.
    ~Driver()
    {
        delete a;
        delete b;
        a = nullptr;
        b = nullptr;
    }

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
        std::printf("[v2] %d %d %d %d %d %d %d %d\n", (*a)(3, 2), (*a)[1], int(*a == *b), int(*a != *b), int(*a < *b), -*a,
            *b - *a, *a * 4);
        std::fflush(stdout);
    }
};
