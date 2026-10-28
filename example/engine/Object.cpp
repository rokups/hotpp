// Out-of-line definitions: the shared library uses the copy in the executable.
#include "Object.h"

int Object::live = 0;

Object::Object() { ++live; }
Object::~Object() { --live; }
void Object::AddRef() { ++refs; }
void Object::Release()
{
    if (--refs == 0) delete this;
}
