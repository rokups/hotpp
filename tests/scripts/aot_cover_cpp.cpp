// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Cover script for optimized AOT code, written in C++ (parity test). The classes have field
// initializers, a script virtual function with an override, a destructor, and a constructor
// that throws. The destructor of CppBoss prints the facts of the run.
#include "Actor.h"

#include <cstdio>
#include <stdexcept>
#include <string>

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

class CppBoss : public Actor
{
public:
    CppBoss() : Actor("CppBoss") {}
    ~CppBoss()
    {
        std::printf("[aot_cover_cpp] CppBoss power=%d attack=%d caught=%d ticks=%d label=%s\n", power, attack, caught,
            ticks, label.c_str());
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
        }
        ticks += 1;
    }

    int power = 0;
    int attack = 0;
    int caught = 0;
    int ticks = 0;
    std::string label = "cpp";
};
