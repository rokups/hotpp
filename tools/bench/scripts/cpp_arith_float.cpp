// Case: floating point arithmetic loop.
// The .cpp twin of arith_float.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchArithFloatCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        float x = 0.5f;
        double d = 0.0;
        for (int i = 0; i < n; i++)
        {
            x = x * 0.999f + 0.5f;
            d = d + x * 0.25;
        }
        return static_cast<int64_t>(d);
    }
};
