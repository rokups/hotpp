// Case: script to native call. Run calls the native method NativeAdd in a loop.
// The .cpp twin of call_native.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchCallNativeCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += NativeAdd(i, 1);
        return s;
    }
};
