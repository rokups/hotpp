// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Generation 1 of the abstract classes as a plain C++ script.
// Gate is concrete. Mid implements Area.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

// Shape has a pure virtual method. Square implements it.
class Shape : public Actor
{
public:
    Shape() : Actor("Shape") {}

    virtual int Area() = 0;

    int Twice() { return Area() * 2; }
};

class Square : public Shape
{
public:
    int Area() override { return side * side; }

    void Tick(float dt) override
    {
        area = Area();
        twice = Twice();
    }

    int side = 3;
    int area = 0;
    int twice = 0;
};

// Mid implements Area in generation 1 only.
class Mid : public Shape
{
public:
    int Area() override { return 4; }

    void Tick(float dt) override { val = Area(); }

    int val = 0;
};

// Gate has the virtual method Power.
class Gate : public Actor
{
public:
    Gate() : Actor("Gate") {}

    virtual int Power() { return 5; }

    void Tick(float dt) override
    {
        ticks = ticks + 1;
        last = Power();
    }

    int ticks = 0;
    int last = 0;
};
