// Generation 1: C array fields. A field keeps its array type in the state.
#include "Actor.h"

#include <functional>
#include <string>
#include <vector>

// Module variables and a static data member of an array type.
int table[4] = {1, 2, 3, 4};

class Buf : public Actor
{
public:
    Buf() : Actor("Buf") { names[0] = "first"; }

    void Tick(float dt) override
    {
        for (int i = 0; i < 4; ++i)
            data[i] += i + 1;
        grid[1][2] += 5;
        total = sum();
        table[0] += 1;
        hits[1] += 1;
        rows[1].push_back(lut[2]);
        Move(Vec3(dt, 0, 0));
    }

    int sum() const
    {
        int s = 0;
        for (int v : data)
            s += v;
        return s + static_cast<int>(sizeof(data) / sizeof(data[0]));
    }

    int data[4] = {10, 20, 30, 40};
    int grid[2][3];
    std::string names[2];
    int total = 0;
    const int lut[3] = {1, 2, 3};
    const std::string tags[2] = {"x", "y"};
    std::function<void()> handlers[2];
    std::vector<int> rows[2];
    static int hits[3];
};

int Buf::hits[3] = {5, 6, 7};
