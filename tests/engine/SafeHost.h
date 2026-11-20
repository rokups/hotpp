// A native class with a member template, for the tests of "x?.M<T>()".
#pragma once

class SafeHost
{
public:
    template <class T> int Size() const { return static_cast<int>(sizeof(T)); }
    template <class T> int Sum(int a, int b) const { return a + b + static_cast<int>(sizeof(T)); }
};
