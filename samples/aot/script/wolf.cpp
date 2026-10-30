// The script of the sample as a plain C++ file. Wolf derives from the host class Creature
// and overrides Tick. A normal C++ build compiles this file. The tool sppc also translates it.
// A .cpp script has no string interpolation, so Tick formats the line with snprintf.
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
        char buffer[128];
        std::snprintf(buffer, sizeof buffer, "%s: bites=%d hp=%03d", this->Name.c_str(), this->bites, this->Hp);
        std::string line = buffer;
        std::printf("[wolf] %s\n", line.c_str());
    }

    int bites = 0;
};
