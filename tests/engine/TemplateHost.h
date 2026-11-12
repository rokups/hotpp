// An actor with a member template, as an engine object with GetSubsystem<T>().
#pragma once
#include "Actor.h"

class TemplateHost : public Actor
{
public:
    template <class T> int Size() const { return static_cast<int>(sizeof(T)); }
};
