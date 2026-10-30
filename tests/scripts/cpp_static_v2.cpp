// Generation 2 of the script class with static members as a plain C++ script.
// The static data member count changes from int to float. A convert hook converts the value.
// The class has a new static data member and a new static method.
#include "Actor.h"

// The method Tick calls this function. It is defined after the class.
int tally();

class Counter : public Actor
{
public:
    Counter() : Actor("Counter") {}

    void Tick(float dt) override
    {
        count += 1.0f;
        this->hits += 2;
        extra += 1;
        last = plus(scale(static_cast<int>(count)), extra);
        via_free = tally();
        Move(Vec3(dt, 0, 0));
    }

    int Peek()
    {
        int (*fn)(int) = &Counter::scale;
        return fn(static_cast<int>(Counter::count)) + kFactor;
    }

    static int scale(int v) { return v * kFactor; }

    // A new static method.
    static int plus(int a, int b = 1) { return a + b; }

    // The convert hook of the static data member count.
    static float convert_count(int old) { return static_cast<float>(old) + 0.5f; }

    int last = 0;
    int via_free = 0;

    static float count;
    static inline int hits = 5;
    // A new static data member: it takes its initializer.
    static inline int extra = 7;
    static constexpr int kFactor = 3;
};

float Counter::count = 10.0f;

int tally()
{
    return Counter::scale(static_cast<int>(Counter::count)) + Counter::hits;
}

int snapshot = static_cast<int>(Counter::count) + 1;
