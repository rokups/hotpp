// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The registration of a reloadable lambda at module load (spike).
//
// The lowering of a lambda gives a tag struct with the static member "key", and a call of
// make_lambda<Tag, Sig>(caps, f). The call instantiates LambdaReg<Tag, Sig, Caps, F>. A static
// member of this class template adds the record to a list of the module when the module loads.
// This happens also when the lambda never runs. script_register gives the list to the registry
// with flush_lambdas.
#pragma once
#include "hot/runtime/hidden.h"

#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace hot
{

// The registration of one lambda. "key" has static storage. "sig" is typeid(Sig).name().
struct LambdaRecord
{
    const char* key = nullptr;
    const char* sig = nullptr;
    void* entry = nullptr; // R (*)(Caps&, P...)
};

namespace detail
{
// The lambdas that the module registered since the last flush. Each module has its own list: the
// function is inline and the module is built with hidden visibility.
inline std::vector<LambdaRecord>& pending_lambdas()
{
    static std::vector<LambdaRecord> list;
    return list;
}

// The entry of a lambda: the lambda type has no state, so the call needs no object of it.
template <class Caps, class F, class R, class... P> R lambda_entry(Caps& caps, P... p)
{
    static_assert(std::is_empty<F>::value, "the lambda of the lowering has no capture");
    alignas(F) static unsigned char storage[1];
    return (*reinterpret_cast<F*>(storage))(caps, static_cast<P&&>(p)...);
}

template <class Tag, class Sig, class Caps, class F> struct LambdaReg;
template <class Tag, class R, class... P, class Caps, class F> struct LambdaReg<Tag, R(P...), Caps, F>
{
    static bool add()
    {
        pending_lambdas().push_back(
            {Tag::key, typeid(R(P...)).name(), reinterpret_cast<void*>(&lambda_entry<Caps, F, R, P...>)});
        return true;
    }
    static const bool registered;
};
template <class Tag, class R, class... P, class Caps, class F>
const bool LambdaReg<Tag, R(P...), Caps, F>::registered = LambdaReg<Tag, R(P...), Caps, F>::add();

// Uses the address of the flag, so the compiler instantiates the static member.
inline void touch(const void*) {}
} // namespace detail

// The capture block: a tuple of decayed values.
template <class... T> std::tuple<std::decay_t<T>...> caps(T&&... v)
{
    return std::tuple<std::decay_t<T>...>(std::forward<T>(v)...);
}

// Spike: the result is the capture block. The registration is the point of the call.
template <class Tag, class Sig, class Caps, class F> Caps make_lambda(Caps c, F)
{
    detail::touch(&detail::LambdaReg<Tag, Sig, Caps, F>::registered);
    return c;
}

// script_register calls it. It gives the lambdas of the module to the registry.
template <class Reg> void flush_lambdas(Reg& reg)
{
    std::vector<LambdaRecord> list;
    list.swap(detail::pending_lambdas());
    for (const LambdaRecord& r : list)
        reg.add_lambda(r);
}

} // namespace hot
