// Generation 2 of a fluent (builder) style script class.
// A method returns "Counter&" or "Counter*": the address of a script object is fixed for its life.
// A normal C++ build compiles this file. The tool hotc also translates it.
#include "Actor.h"

class Counter : public Actor
{
public:
    Counter() : Actor("Counter") {}

    // The fluent method returns the object itself.
    Counter& add(int v)
    {
        n += v * 10; // the new body
        return *this;
    }

    const Counter& peek() const { return *this; }
    Counter* self() { return this; }

    int n = 0;
};

// The module variable keeps the pointer to the object across a reload.
Counter* counter = nullptr;

class Builder : public Actor
{
public:
    Builder() : Actor("Builder") {}
    // The builder owns the object of the module variable. Without this, it stays alive after the test.
    ~Builder()
    {
        delete counter;
        counter = nullptr;
    }

    void Tick(float dt) override
    {
        (void)dt;
        if (!counter)
            counter = new Counter();
        Counter& c = *counter;
        c.add(1).add(2);
        if (&counter->add(0).add(0) == counter)
            same_ref++;
        if (counter->self() == counter && &counter->peek() == counter)
            same_ptr++;
        total = counter->n;
    }

    int same_ref = 0;
    int same_ptr = 0;
    int total = 0;
};
