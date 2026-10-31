// Case: a call of a small script method on this object (the live check at the method entry).
// The .cpp twin of method_entry.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchEntryCpp : public BenchBase
{
public:
    int k = 1;

    int Add(int x)
    {
        return x + k;
    }

    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += Add(i ^ static_cast<int>(s & 7));
        return s;
    }
};
