// Generation 1 of a script class with lambdas that use the object, as a plain C++ script.
// A normal C++ build compiles this file. The tool sppc also translates it.
#include "Actor.h"

#include <functional>

int calls = 0;

class Lamp : public Actor
{
public:
    // A lambda in a constructor body.
    Lamp() : Actor("Lamp")
    {
        auto init = [=] { return base_level * 2; };
        level = init();
    }

    void Tick(float dt) override
    {
        // The capture [this] with a field, a script method and a native base member.
        auto by_this = [this, dt] { bump(1); Move(Vec3(dt, 0, 0)); return count; };
        by_this();
        // The capture [=] with a field.
        auto by_copy = [=] { return count + level; };
        sum = by_copy();
        // The capture [&] with a field and a method call.
        auto by_ref = [&] { count += 10; bump(2); Move(Vec3(0, dt, 0)); };
        by_ref();
        // A generic lambda.
        auto generic = [this](auto v) { return count + v; };
        gen = generic(100);
        // A nested lambda.
        auto outer = [this] { auto inner = [this] { return count * 2; }; return inner() + level; };
        nested = outer();
        // A lambda that uses a module variable and "this" as a value.
        auto module_use = [this] { calls += 1; return self_is_set(this); };
        flag = module_use();
        // A lambda in a field. A reload copies the field, and the old lambda stays.
        if (!stored)
            stored = [this] { return count + 1000; };
        stored_result = stored();
        // A lambda with [&] and a lambda with an init-capture of "this". The method returns before
        // the host calls them. They keep the object, not a local of the method.
        if (!kept_ref)
            kept_ref = [&] { return count * 3; };
        if (!kept_init)
            kept_init = [self = this] { return self->count + 5; };
    }

    void bump(int n) { count += n; }
    bool self_is_set(Actor* a) { return a != nullptr; }

    int base_level = 3;
    int level = 0;
    int count = 0;
    int sum = 0;
    int gen = 0;
    int nested = 0;
    bool flag = false;
    int stored_result = 0;
    std::function<int()> stored;
    std::function<int()> kept_ref;
    std::function<int()> kept_init;
    // A lambda in a field initializer.
    std::function<int()> from_init = [this] { return count + level; };
};
