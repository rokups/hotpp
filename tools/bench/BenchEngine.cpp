#include "BenchEngine.h"

int64_t BenchBase::Run(int n)
{
    return n;
}

int BenchBase::Hook(int x)
{
    return x;
}

int64_t BenchBase::DriveHook(int n)
{
    int64_t s = 0;
    for (int i = 0; i < n; ++i)
        s += Hook(i);
    return s;
}

int BenchBase::NativeAdd(int a, int b)
{
    return a + b;
}
