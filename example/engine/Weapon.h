// A native class that scripts hold through a std::shared_ptr handle.
#pragma once
#include <string>
#include <utility>

struct Weapon {
    int dmg = 0;
    std::string name;
    Weapon(int d, std::string n) : dmg(d), name(std::move(n)) {}
};
