// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The closure that leaves script code and the guard of a script "finally" block.
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/hidden.h"
#include "hot/runtime/threads.h"

#include <cstdint>
#include <functional>
#include <utility>

// A hint that a condition is rarely true.
#if defined(__clang__) || defined(__GNUC__)
    #define HOT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
// The MSVC form is the plain condition.
    #define HOT_UNLIKELY(x) (x)
#endif

namespace hot
{

namespace detail
{
// One pin of a generation (hot::pin). A copy holds its own pin. Generation 0 has no pin.
struct GenerationPin
{
    uint32_t gen = 0;
    HOT_HIDDEN explicit GenerationPin(uint32_t g) : gen(g)
    {
        if (gen)
            pin(gen);
    }
    HOT_HIDDEN GenerationPin(const GenerationPin& o) : gen(o.gen)
    {
        if (gen)
            pin(gen);
    }
    HOT_HIDDEN GenerationPin(GenerationPin&& o) noexcept : gen(o.gen) { o.gen = 0; }
    GenerationPin& operator=(const GenerationPin&) = delete;
    HOT_HIDDEN ~GenerationPin()
    {
        if (gen)
            unpin(gen);
    }
};

// The lambda of a closure with its pin and its guard. A std::function holds it directly, so a
// call through the std::function is one indirect call. The pin is the first member, so it goes
// last: the lambda goes first (its captures can use the generation code).
template <class F, class R, class... A> struct GuardedFn
{
    GenerationPin pin;
    const uint32_t* live; // null: the guard compares the generation with active_generation
    const char* what;
    mutable F fn;
    HOT_HIDDEN R operator()(A... a) const
    {
        ScriptEntry entry; // design 6.3: the guard reads the live flag or active_generation
        if (HOT_UNLIKELY(live ? !*live : pin.gen != active_generation))
        {
            stale_call(what ? what : "closure");
            return R();
        }
        return fn(std::forward<A>(a)...);
    }
};
} // namespace detail

// A closure that leaves script code (section 4.11). It holds one pin of its
// generation for its whole life. Each call checks the generation guard: when the
// generation is not live, the call logs a stale call and returns R(). The lambda
// body does not run. A copy of the closure holds its own pin.
//
// The closure converts to std::function and to an engine delegate type D that has
// Bind(std::function<Sig>). The std::function holds the guarded lambda, not the closure,
// so its call has one indirect call and not two.
// The closure code lives in the generation module. Destroy the last copy on the
// main thread, before the frame boundary at which the generation can unload.
template <class Sig> class Closure;

template <class R, class... A> class Closure<R(A...)>
{
public:
    using Function = std::function<R(A...)>;

    HOT_HIDDEN Closure() {}

    // Without a live flag the guard compares the generation with active_generation.
    template <class F>
    HOT_HIDDEN Closure(uint32_t generation, const char* what, F fn)
        : Closure(generation, nullptr, what, std::move(fn))
    {
    }

    // "live" is the live flag of the module (Registry::set_live_flag). The guard reads it.
    template <class F>
    HOT_HIDDEN Closure(uint32_t generation, const uint32_t* live, const char* what, F fn)
        : fn_(detail::GuardedFn<F, R, A...>{detail::GenerationPin(generation), live, what, std::move(fn)})
        , gen_(generation)
    {
    }

    // A copy copies the guarded lambda, and so its pin.
    HOT_HIDDEN Closure(const Closure& o) = default;

    HOT_HIDDEN Closure(Closure&& o) noexcept
        : fn_(std::move(o.fn_))
        , gen_(o.gen_)
    {
        o.fn_ = nullptr;
        o.gen_ = 0;
    }

    HOT_HIDDEN Closure& operator=(Closure o) noexcept
    {
        std::swap(fn_, o.fn_);
        std::swap(gen_, o.gen_);
        return *this;
    }

    HOT_HIDDEN ~Closure() = default;

    HOT_HIDDEN R operator()(A... a) const
    {
        if (HOT_UNLIKELY(!fn_))
        {
            stale_call("closure");
            return R();
        }
        return fn_(std::forward<A>(a)...);
    }

    HOT_HIDDEN explicit operator bool() const { return gen_ != 0; }
    HOT_HIDDEN uint32_t generation() const { return gen_; }

    // A std::function that holds a copy of the guarded lambda (and so one more pin). An
    // empty closure gives a std::function that logs a stale call.
    HOT_HIDDEN Function to_function() const& { return fn_ ? fn_ : Function(Closure()); }
    HOT_HIDDEN Function to_function() && { return fn_ ? take() : Function(Closure()); }

    // The conversion to std::function. The three forms match the reference binding of the
    // constructor template of std::function. So "std::function<Sig> f = closure" selects this
    // conversion, not that constructor: a template loses a tie.
    HOT_HIDDEN operator Function() & { return to_function(); }
    HOT_HIDDEN operator Function() const& { return to_function(); }
    HOT_HIDDEN operator Function() && { return std::move(*this).to_function(); }

    // Conversion to an engine delegate type that binds a std::function.
    template <class D, std::enable_if_t<detail::has_bind<D, Function>::value, int> = 0> HOT_HIDDEN operator D() const
    {
        D d;
        d.Bind(to_function());
        return d;
    }

private:
    HOT_HIDDEN Function take()
    {
        Function f = std::move(fn_);
        fn_ = nullptr;
        gen_ = 0;
        return f;
    }

    Function fn_; // a detail::GuardedFn, or empty
    uint32_t gen_ = 0;
};

// The guard of a script "finally" block. The translator writes
//   { hot::Finally __fin_N([&] { body }); try { ... } catch ... }
// The destructor runs the body on each path out of the outer block: the normal end,
// a return, a break, a continue and an exception.
//
// Rule: the body must not let an exception out of the destructor. During unwinding, such
// an exception calls std::terminate. So the destructor catches and drops each exception
// from the body. The exception from the try block (if any) continues.
template <class F> class Finally
{
public:
    HOT_HIDDEN explicit Finally(F f)
        : f_(std::move(f))
    {
    }
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;
    HOT_HIDDEN ~Finally()
    {
        try
        {
            f_();
        }
        catch (...)
        {
            // Dropped. See the rule above.
        }
    }

private:
    F f_;
};

namespace detail
{
template <class M> struct SigOf;
template <class C, class R, class... A> struct SigOf<R (C::*)(A...) const>
{
    using type = R(A...);
};
template <class C, class R, class... A> struct SigOf<R (C::*)(A...)>
{
    using type = R(A...);
};
} // namespace detail

// The translator wraps each arrow lambda in this call. "what" names the lambda
// in the stale call log.
template <class F> HOT_HIDDEN auto make_closure(uint32_t generation, const char* what, F fn)
{
    using Sig = typename detail::SigOf<decltype(&F::operator())>::type;
    return Closure<Sig>(generation, what, std::move(fn));
}

// The form of the translator: "live" is the live flag of the module.
template <class F> HOT_HIDDEN auto make_closure(uint32_t generation, const uint32_t* live, const char* what, F fn)
{
    using Sig = typename detail::SigOf<decltype(&F::operator())>::type;
    return Closure<Sig>(generation, live, what, std::move(fn));
}

} // namespace hot
