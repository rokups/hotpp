// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Generation 2 of a chain of three script classes: Leaf derives from Mid, and Mid derives from Base.
// Base has the new field armor and the new virtual method Extra. Mid and Leaf override Extra.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

// The number of Tick calls on an object that is a Mid.
int mids = 0;

class Base : public Actor
{
public:
    Base() : Actor("Base") {}

    virtual int Step() { return 1; }

    virtual int Extra() { return 0; }

    void Tick(float dt) override
    {
        (void)dt;
        total += Step() + Extra();
    }

    int total = 0;
    int armor = 4;
};

class Mid : public Base
{
public:
    Mid() : Base() {}

    int Step() override { return 10; }

    int Extra() override { return 1000; }

    int mid = 5;
};

class Leaf : public Mid
{
public:
    Leaf() : Mid() {}

    int Step() override { return Mid::Step() + 200; }

    int Extra() override { return Mid::Extra() + 1; }

    void Tick(float dt) override
    {
        Mid::Tick(dt);
        leaf += 1;
        Base* b = this;
        if (dynamic_cast<Mid*>(b) != nullptr)
            mids++;
    }

    int leaf = 6;
    int leaf2 = 7;
};
