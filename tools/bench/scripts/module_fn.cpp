// Case: a call of a module function and of a static method (a .cpp script).
// A .hot script has no free functions and no static methods, so this case is a .cpp script.
#include "BenchEngine.h"

#include <cstdint>

// A module variable. The module function reads it.
int bias = 1;

static int add_bias(int x)
{
    return x + bias;
}

class BenchModFn : public BenchBase
{
public:
    static int Twice(int x)
    {
        return x * 2;
    }

    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += add_bias(i) + Twice(i & 7);
        return s;
    }
};
