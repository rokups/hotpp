// Case: pointer dereference and null check along a chain of four objects.
// The .cpp twin of handles.hot. A .cpp script has no handles, so it uses raw pointers. A raw pointer
// has no generation check. The null check stays, as in the .hot script.
#include "BenchEngine.h"

#include <cstdint>

class BenchNodeCpp : public BenchBase
{
public:
    int v = 0;
    BenchNodeCpp* next = nullptr;
};

class BenchHandlesCpp : public BenchBase
{
public:
    BenchNodeCpp* head = nullptr;

    ~BenchHandlesCpp()
    {
        while (head)
        {
            BenchNodeCpp* n = head->next;
            delete head;
            head = n;
        }
    }

    int64_t Run(int n) override
    {
        if (head == nullptr)
        {
            head = new BenchNodeCpp();
            head->v = 1;
            BenchNodeCpp* cur = head;
            for (int j = 1; j < 4; j++)
            {
                cur->next = new BenchNodeCpp();
                cur->next->v = j + 1;
                cur = cur->next;
            }
        }
        int64_t s = 0;
        BenchNodeCpp* p = head;
        for (int i = 0; i < n; i++)
        {
            if (p == nullptr)
                p = head;
            s += p->v;
            p = p->next;
        }
        return s;
    }
};
