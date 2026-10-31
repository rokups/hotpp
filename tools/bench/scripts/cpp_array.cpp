// Case: array index loop. std::array has no bounds check in operator[].
// The .cpp twin of array.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <array>
#include <cstdint>

class BenchArrayCpp : public BenchBase
{
public:
    std::array<int, 256> arr{};

    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            int k = i & 255;
            arr[k] = (arr[(k + 1) & 255] + i) & 65535;
            s += arr[k];
        }
        return s;
    }
};
