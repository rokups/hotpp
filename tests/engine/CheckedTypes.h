// Host containers for the tests of the stub attribute "checked_index". They have no bounds test
// of their own: the runtime test shows that hot::index adds the test.
#pragma once

#include <cstddef>
#include <vector>

namespace hostns2
{

// A class template with type parameters only: the stub "hostns2.Ring<*>" names it.
template <class T> class Ring
{
public:
    explicit Ring(std::size_t n = 0) : data_(n) {}
    std::size_t size() const { return data_.size(); }
    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }

private:
    std::vector<T> data_;
};

// A plain class: the stub "hostns2.Plain" names it.
class Plain
{
public:
    std::size_t size() const { return 4; }
    int& operator[](std::size_t i) { return data_[i]; }
    const int& operator[](std::size_t i) const { return data_[i]; }

private:
    int data_[4] = {};
};

// A fixed-size array with a size parameter: the module attribute array names it.
template <class T, std::size_t N> class Fixed
{
public:
    std::size_t size() const { return N; }
    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }

private:
    T data_[N] = {};
};

// A namespace that starts with "__" and is not inline stays in the qualified name. An inline
// namespace does not.
namespace __detail
{
template <class T> class Deep : public Ring<T>
{
};
} // namespace __detail

inline namespace v1
{
template <class T> class Versioned : public Ring<T>
{
};
} // namespace v1

} // namespace hostns2
