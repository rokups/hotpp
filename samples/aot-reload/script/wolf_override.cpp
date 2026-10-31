// The run-time override of the unit wolf as a plain C++ file. The program compiles this file
// while it runs. Wolf has one more field, howls. The fields bites and the base state stay.
#include "Creature.h"

#include <cstdio>
#include <string>

class Wolf : public Creature
{
public:
    Wolf() : Creature(std::string("wolf")) {}

    void Tick(float dt) override
    {
        (void)dt;
        this->bites = this->bites + 1;
        this->howls = this->howls + 1;
        this->Hp = this->Hp - 1;
        std::printf("[override] %s: bites=%d hp=%d howls=%d\n", this->Name.c_str(), this->bites, this->Hp, this->howls);
    }

    int bites = 0;
    int howls = 0;
};
