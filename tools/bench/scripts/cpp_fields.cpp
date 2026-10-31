// Case: field read and write loop.
// The .cpp twin of fields.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchFieldsCpp : public BenchBase
{
public:
    int a = 1;
    int b = 2;
    float f = 0.5f;

    int64_t Run(int n) override
    {
        for (int i = 0; i < n; i++)
        {
            a = (a + i) & 65535;
            b = b ^ a;
            f = f + 0.25f;
        }
        return a + b + static_cast<int64_t>(f);
    }
};
