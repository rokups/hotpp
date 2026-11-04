// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Cover script for optimized AOT code, written in C++ (parity test). The classes have field
// initializers, a script virtual function with an override, a destructor, and a constructor
// that throws. The destructor of CppBoss prints the facts of the run.
#include "Actor.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class CppUnit : public Actor
{
public:
    CppUnit() : Actor("CppUnit") {}
    virtual int Power() { return base_power; }
    int Attack() { return Power() * 10; }

    int base_power = 2;
};

class CppElite : public CppUnit
{
public:
    int Power() override { return CppUnit::Power() + 100; }
};

class CppFussy
{
public:
    explicit CppFussy(int v) : n(v)
    {
        if (v > 1)
            throw std::runtime_error("fussy");
    }
    ~CppFussy() { std::printf("[aot_cover_cpp] CppFussy %d gone\n", n); }

    int n = 0;
};

// Reads a field of another object through a pointer, a reference and a const reference, and
// a field of this in a lambda. The optimized shell has the fields, so no rewrite is needed.
class CppReader : public Actor
{
public:
    CppReader() : Actor("CppReader") {}
    // Operators: the optimized shell has them as members, and the bodies are the members.
    CppReader& operator+=(int v)
    {
        mine += v;
        return *this;
    }
    int operator()(int k) const { return mine + k; }
    int ReadPtr(CppUnit* u) { return u->base_power; }
    int ReadRef(CppUnit& u) { return u.base_power; }
    int ReadConst(const CppUnit& u) { return u.base_power; }
    int ReadLambda()
    {
        auto f = [this]() { return mine + this->mine; };
        return f();
    }

    int mine = 4;
};

// A constructor and a method whose parameters have the name inside the parentheses: a function
// pointer and a pointer to an array.
class CppApply : public Actor
{
public:
    CppApply(int (*f)(int), int (*p)[3]) : Actor("CppApply"), seed(f(p[0][1])) {}
    int Run(int (*g)(int), int (*q)[3]) { return g(q[1][2]) + seed; }

    int seed = 0;
};

class CppBoss : public Actor
{
public:
    CppBoss() : Actor("CppBoss") {}
    ~CppBoss()
    {
        std::printf("[aot_cover_cpp] CppBoss power=%d attack=%d caught=%d ticks=%d label=%s\n", power, attack, caught,
            ticks, label.c_str());
        std::printf("[aot_cover_cpp] CppBoss hits=%d row=%d op=%d\n", hits.load(), row[1][1], op(7));
        std::printf("[aot_cover_cpp] CppBoss applied=%d owned=%d\n", applied, *owned[0]);
    }

    void Tick(float) override
    {
        if (ticks == 0)
        {
            CppElite* e = new CppElite();
            power = e->Power();
            attack = e->Attack();
            try
            {
                CppFussy* f = new CppFussy(5);
                (void)f;
            }
            catch (const std::runtime_error&)
            {
                caught += 1;
            }
            CppFussy* g = new CppFussy(1);
            (void)g;
            CppApply* a = new CppApply([](int v) { return v * 2; }, grid);
            applied = a->Run([](int v) { return v + 1; }, grid);
            owned.push_back(std::make_unique<int>(7));
            CppReader* r = new CppReader();
            *r += 1;
            const CppReader& cr = *r; // a call of a const operator through a const reference
            power += cr(0) - 5; // the operators change nothing in the output
        }
        ticks += 1;
        hits += 1;
        row = grid;
        op = [](int v) { return v * 2; };
    }

    int power = 0;
    int attack = 0;
    int caught = 0;
    int ticks = 0;
    int applied = 0;
    // A container of a move-only type: it looks copyable, but the copy does not compile. It moves.
    std::vector<std::unique_ptr<int>> owned;
    std::string label = "cpp";
    // A field that has no copy constructor must construct in place.
    std::atomic<int> hits = {0};
    // The declarator has the name inside the parentheses: a pointer to an array, a function pointer.
    int grid[2][3] = {{1, 2, 3}, {4, 5, 6}};
    int (*row)[3] = nullptr;
    int (*op)(int) = nullptr;
};
