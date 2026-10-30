// Generation 1 of the script as a plain C++ file. Wolf derives from the host class Creature.
// A normal C++ build compiles this file. The tool hotc also translates it.
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
        this->Hp = this->Hp - 1;
        std::printf("[v1] %s: bites=%d hp=%d\n", this->Name.c_str(), this->bites, this->Hp);
    }

    int bites = 0;
};
