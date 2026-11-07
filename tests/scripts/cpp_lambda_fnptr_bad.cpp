// A lambda that uses a module variable has the stale guard. It cannot become a function pointer:
// the C++ compile gives a clear error.
#include "Actor.h"

int hits = 0;

class Bad : public Actor
{
public:
    Bad() : Actor("Bad") {}

    void Tick(float) override
    {
        int (*fp)() = [] { return hits; };
        hits = fp();
    }
};
