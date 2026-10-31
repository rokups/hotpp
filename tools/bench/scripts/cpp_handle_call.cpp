// Case: a call of a script method through a pointer to another script object.
// The .cpp twin of handle_call.hot. A .cpp script has no handles, so it uses a raw pointer.
#include "BenchEngine.h"

#include <cstdint>

class BenchAdderCpp : public BenchBase
{
public:
    int k = 3;

    int Add(int x)
    {
        return x + k;
    }
};

class BenchHandleCallCpp : public BenchBase
{
public:
    BenchAdderCpp* a = nullptr;

    ~BenchHandleCallCpp()
    {
        delete a;
    }

    int64_t Run(int n) override
    {
        if (a == nullptr)
            a = new BenchAdderCpp();
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += a->Add(i ^ static_cast<int>(s & 7));
        return s;
    }
};
