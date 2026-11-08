// Generation 2 of a C++ script class with move-only fields. It adds the field "ticks". The field
// "level" changes its type. The convert hook takes the old value by a move.
#include "Actor.h"

#include <memory>
#include <vector>

class Holder : public Actor
{
public:
    Holder() : Actor("Holder") {}

    void Tick(float) override
    {
        ticks += 1;
        many.push_back(std::make_unique<int>(4));
    }

    std::unique_ptr<int> one = std::make_unique<int>(70);
    std::vector<std::unique_ptr<int>> many;
    int ticks = 0;
    std::shared_ptr<int> level;

    std::shared_ptr<int> convert_level(std::unique_ptr<int> old) { return std::shared_ptr<int>(std::move(old)); }
};
