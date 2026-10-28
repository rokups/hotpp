// The argument "ref e" or "out e" of the script.
#pragma once

namespace script {

// The argument "ref e" or "out e" of the script. It holds a reference to the operand.
// Clang picks the form from the native signature: a "T&" parameter gets the object,
// and a "T*" parameter (or a "T*" field) gets its address.
template <class T>
struct RefArg {
    T& ref;
    constexpr operator T&() const noexcept { return ref; }
    constexpr operator T*() const noexcept { return &ref; }
};
template <class T>
constexpr RefArg<T> ref_arg(T& v) noexcept
{
    return RefArg<T>{v};
}
// A temporary has no address that the callee can keep.
template <class T>
void ref_arg(const T&&) = delete;

}  // namespace script
