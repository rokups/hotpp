// Case: read and write of a module variable.
// The .cpp twin of module_var.hot: the same work as the .hot script.
#include "BenchEngine.h"

#include <cstdint>

int counter = 0;
int step = 3;

class BenchModVarCpp : public BenchBase
{
public:
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            counter = (counter + step + i) & 65535;
            s += counter;
        }
        return s;
    }
};
