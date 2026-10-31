// Case: a loop with a method call that changes fields in each iteration.
// The .cpp twin of method_loop.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchMethodLoopCpp : public BenchBase
{
public:
    int a = 0;
    int b = 0;

    void Step(int d)
    {
        a = (a + d) & 65535;
        b = b + a;
        if (b > 1000000)
            b = b - 1000000;
    }

    int64_t Run(int n) override
    {
        for (int i = 0; i < n; i++)
            Step(i);
        return a + b;
    }
};
