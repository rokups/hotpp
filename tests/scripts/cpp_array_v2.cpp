// Generation 2: the arrays keep their values. "data" gets a new size (it resets) and "extra" is new.
#include "Actor.h"

#include <functional>
#include <string>
#include <vector>

int table[4] = {1, 2, 3, 4};

class Buf : public Actor
{
public:
    Buf() : Actor("Buf") { names[0] = "first"; }

    void Tick(float dt) override
    {
        for (int i = 0; i < 6; ++i)
            data[i] += 100;
        grid[1][2] += 1;
        extra[3] = 7;
        table[0] += 10;
        hits[1] += 10;
        Move(Vec3(dt, 0, 0));
    }

    int sum() const
    {
        int s = 0;
        for (int v : data)
            s += v;
        return s;
    }

    int data[6] = {10, 20, 30, 40};
    int grid[2][3];
    std::string names[2];
    int total = 0;
    const int lut[3] = {1, 2, 3};
    const std::string tags[2] = {"x", "y"};
    std::function<void()> handlers[2];
    std::vector<int> rows[2];
    int extra[8] = {1, 2, 3};
    static int hits[3];
};

int Buf::hits[3] = {5, 6, 7};
