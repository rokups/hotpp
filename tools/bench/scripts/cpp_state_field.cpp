// Case: field read and write of another script object through a pointer (the state indirection).
// The .cpp twin of state_field.hot. A .cpp script has no handles, so it uses a raw pointer.
#include "BenchEngine.h"

#include <cstdint>

class BenchCellCpp : public BenchBase
{
public:
    int x = 0;
    int y = 1;
};

class BenchStateFieldCpp : public BenchBase
{
public:
    BenchCellCpp* c = nullptr;

    ~BenchStateFieldCpp()
    {
        delete c;
    }

    int64_t Run(int n) override
    {
        if (c == nullptr)
            c = new BenchCellCpp();
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            c->x = (c->x + i) & 65535;
            c->y = c->y ^ c->x;
            s += c->y;
        }
        return s;
    }
};
