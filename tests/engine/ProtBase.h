// A native class with a protected non-virtual member, for the tests of the friend state struct.
#pragma once
#include "Actor.h"

class ProtBase : public Actor
{
public:
    int Value() const { return value_; }

protected:
    int Poke(int n)
    {
        value_ += n;
        return value_;
    }

private:
    int value_ = 0;
};
