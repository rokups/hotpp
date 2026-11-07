// Generation 2 of the class that keeps lambdas in fields. The field "pad" is gone. The fields
// with lambdas keep the lambdas of generation 1: the method Tick does not set them again.
#include "Actor.h"

#include <functional>
#include <string>

int hits = 0;

class Keeper : public Actor
{
public:
    Keeper() : Actor("Keeper") {}

    void Tick(float) override {}

    int count = 7;
    std::string label = "keeper";
    std::function<int()> read_count;
    std::function<int()> touch_module;
    std::function<std::string()> read_label;
    std::function<int(Actor*)> peek;
    std::function<void(int)> poke;
    std::function<int()> pure;
};
