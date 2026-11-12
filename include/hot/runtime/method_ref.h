// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The unbound method group "C.M" of a .hot script (doc/lambdas.md, "Method groups").
//
// A MethodRef is a callable that takes the receiver first: (Shell*, P...). An engine API that
// takes a receiver and a handler calls it with the receiver, for example
// "signal.Subscribe(this, C.M)". The call goes through the slot table of the receiver: the
// current generation runs, and an override of M in a script child runs too. The slot number is a
// template argument, so the object holds only the pin of the generation of its code.
//
// A changed signature of M detaches the call: the slot key does not agree (hot::call). A removed
// M is a defunct slot: hot::call reports the script error and gives the default result.
#pragma once

#include "hot/runtime/hidden.h"
#include "hot/runtime/shell.h"
#include "hot/runtime/threads.h"

#include <cstdint>
#include <utility>

namespace hot
{

// "Sig" is the slot signature R(Base*, P...) of M. "Shell" is the shell class of C in the
// generation of the code. "Slot" is the slot of M.
template <class Sig, class Shell, unsigned Slot> class MethodRef;

template <class R, class B, class... P, class Shell, unsigned Slot> class MethodRef<R(B*, P...), Shell, Slot>
{
public:
    // Pins the generation "gen": the code of operator() is in its module.
    HOT_HIDDEN explicit MethodRef(uint32_t gen)
        : gen_(gen)
    {
        if (gen_)
            pin(gen_);
    }

    HOT_HIDDEN MethodRef(const MethodRef& o)
        : gen_(o.gen_)
    {
        if (gen_)
            pin(gen_);
    }

    HOT_HIDDEN MethodRef(MethodRef&& o) noexcept
        : gen_(std::exchange(o.gen_, 0u))
    {
    }

    HOT_HIDDEN MethodRef& operator=(MethodRef o) noexcept
    {
        std::swap(gen_, o.gen_);
        return *this;
    }

    HOT_HIDDEN ~MethodRef()
    {
        if (gen_)
            unpin(gen_);
    }

    HOT_HIDDEN R operator()(Shell* receiver, P... p) const
    {
        return hot::call<R(B*, P...)>(receiver, Slot, std::forward<P>(p)...);
    }

    HOT_HIDDEN uint32_t generation() const { return gen_; }

private:
    uint32_t gen_ = 0;
};

} // namespace hot
