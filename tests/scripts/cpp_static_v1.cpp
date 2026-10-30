// Generation 1 of a script class with static members as a plain C++ script.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

// The method Tick calls this function. It is defined after the class.
int tally();

class Counter : public Actor
{
public:
    Counter() : Actor("Counter") {}

    void Tick(float dt) override
    {
        ++count;                // a bare name
        this->hits += 2;        // an access through "this"
        last = scale(count);    // a call of a static method
        via_free = tally();     // a free function uses the static members
        Move(Vec3(dt, 0, 0));
    }

    // The forms of a use: a qualified name, an address and a constant.
    int Peek()
    {
        int (*fn)(int) = &Counter::scale;
        return fn(Counter::count) + kFactor;
    }

    static int scale(int v) { return v * kFactor; }

    int last = 0;
    int via_free = 0;

    // A static data member. The module variable "Counter__s_count" holds the value.
    static int count;
    // A C++17 inline static data member with an initializer.
    static inline int hits = 5;
    // A constant stays a constant. It has no state.
    static constexpr int kFactor = 3;
};

// The definition outside the class gives the initializer of "count".
int Counter::count = 10;

// A function outside the class body uses the static members.
int tally()
{
    return Counter::scale(Counter::count) + Counter::hits;
}

// A module variable with an initializer that reads a static data member.
int snapshot = Counter::count + 1;
