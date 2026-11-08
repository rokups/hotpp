// Generation 1 of a C++ script class with move-only fields. A reload moves these fields.
#include "Actor.h"

#include <memory>
#include <vector>

class Holder : public Actor
{
public:
    Holder() : Actor("Holder") {}

    void Tick(float) override { many.push_back(std::make_unique<int>(3)); }

    std::unique_ptr<int> one = std::make_unique<int>(7);
    std::vector<std::unique_ptr<int>> many;
    std::unique_ptr<int> level = std::make_unique<int>(2);
};
