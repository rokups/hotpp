// Generation 1 of a class that keeps lambdas in fields, as a plain C++ script.
// Generation 2 removes the large field "pad", so the state block gets smaller. A lambda of
// generation 1 reads the fields with the old offsets. After the reload, the guard must skip it.
#include "Actor.h"

#include <functional>
#include <string>

int hits = 0;

class Keeper : public Actor
{
public:
    Keeper() : Actor("Keeper") {}

    void Tick(float) override
    {
        if (!read_count)
            read_count = [this] { return count + 1; };
        if (!touch_module)
            touch_module = [this] { hits += 1; return hits + count; };
        if (!read_label)
            read_label = [&] { return label + "!"; };
        if (!peek)
            peek = [](Actor* a) { return static_cast<Keeper*>(a)->count * 2; };
        if (!poke)
            poke = [this](int n) mutable { count += n; };
        if (!pure)
            pure = [n = 5] { return n * 10; };
    }

    char pad[4096] = {};
    int count = 7;
    std::string label = "keeper";
    std::function<int()> read_count;
    std::function<int()> touch_module;
    std::function<std::string()> read_label;
    std::function<int(Actor*)> peek;
    std::function<void(int)> poke;
    std::function<int()> pure;
};
