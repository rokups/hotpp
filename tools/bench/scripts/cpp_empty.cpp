// Case: empty method call. The host calls Run(0) many times.
// The .cpp twin of empty.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchEmptyCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        return n;
    }
};
