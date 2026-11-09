// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The closure that leaves script code and the guard of a script "finally" block.
#pragma once
#include "hot/runtime/compat.h"
#include "hot/runtime/errors.h"
#include "hot/runtime/hidden.h"
#include "hot/runtime/threads.h"

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
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

// The operations of a closure box. The box has a type-erased lambda, so the closure itself is
// two words. These are plain function pointers, one table for each lambda type.
template <class R, class... A> struct GuardedOps
{
    R (*invoke)(void*, A...);
    void* (*clone)(const void*);
    void (*destroy)(void*);
    // A std::function that holds a copy of the guarded lambda, or (move) the lambda itself.
    std::function<R(A...)> (*to_function)(void*, bool move);
};

// The header of a box. The closure holds a pointer to it.
template <class R, class... A> struct GuardedBox
{
    const GuardedOps<R, A...>* ops;
};

// The box of one lambda type. "fn" is the GuardedFn that a std::function can hold too.
// The ops functions are not HOT_HIDDEN: the table holds their addresses.
template <class F, class R, class... A> struct GuardedBoxOf : GuardedBox<R, A...>
{
    GuardedFn<F, R, A...> fn;

    explicit GuardedBoxOf(GuardedFn<F, R, A...>&& f)
        : GuardedBox<R, A...>{ops()}
        , fn(std::move(f))
    {
    }

    static R invoke(void* p, A... a) { return static_cast<GuardedBoxOf*>(p)->fn(std::forward<A>(a)...); }
    static void* clone(const void* p) { return new GuardedBoxOf(*static_cast<const GuardedBoxOf*>(p)); }
    static void destroy(void* p) { delete static_cast<GuardedBoxOf*>(p); }
    static std::function<R(A...)> to_function(void* p, bool move)
    {
        GuardedBoxOf* box = static_cast<GuardedBoxOf*>(p);
        if (move)
            return std::function<R(A...)>(std::move(box->fn));
        return std::function<R(A...)>(box->fn);
    }
    static const GuardedOps<R, A...>* ops()
    {
        static const GuardedOps<R, A...> table = {&invoke, &clone, &destroy, &to_function};
        return &table;
    }
};
} // namespace detail

// A closure that leaves script code (section 4.11). It holds one pin of its
// generation for its whole life. Each call checks the generation guard: when the
// generation is not live, the call logs a stale call and returns R(). The lambda
// body does not run.
//
// The closure is two words: a pointer to one heap box and the generation. The box holds the
// guarded lambda (the pin, the guard and the lambda). A copy of the closure clones the box, so
// it holds its own pin. A move gives the box to the new closure. The small size lets a lambda that
// captures a closure fit in the small buffer of a native Signal::Subscribe.
//
// The closure converts to std::function and to an engine delegate type D that has
// Bind(std::function<Sig>). The std::function holds a copy of the guarded lambda, not the closure,
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
        : box_(new detail::GuardedBoxOf<F, R, A...>(
              detail::GuardedFn<F, R, A...>{detail::GenerationPin(generation), live, what, std::move(fn)}))
        , gen_(generation)
    {
    }

    // A copy clones the box, and so the pin.
    HOT_HIDDEN Closure(const Closure& o)
        : box_(o.box_ ? static_cast<detail::GuardedBox<R, A...>*>(o.box_->ops->clone(o.box_)) : nullptr)
        , gen_(o.gen_)
    {
    }

    HOT_HIDDEN Closure(Closure&& o) noexcept
        : box_(o.box_)
        , gen_(o.gen_)
    {
        o.box_ = nullptr;
        o.gen_ = 0;
    }

    HOT_HIDDEN Closure& operator=(Closure o) noexcept
    {
        std::swap(box_, o.box_);
        std::swap(gen_, o.gen_);
        return *this;
    }

    HOT_HIDDEN ~Closure()
    {
        if (box_)
            box_->ops->destroy(box_);
    }

    HOT_HIDDEN R operator()(A... a) const
    {
        if (HOT_UNLIKELY(!box_))
        {
            stale_call("closure");
            return R();
        }
        return box_->ops->invoke(box_, std::forward<A>(a)...);
    }

    HOT_HIDDEN explicit operator bool() const { return gen_ != 0; }
    HOT_HIDDEN uint32_t generation() const { return gen_; }

    // A std::function that holds a copy of the guarded lambda (and so one more pin). An
    // empty closure gives a std::function that logs a stale call.
    HOT_HIDDEN Function to_function() const& { return box_ ? box_->ops->to_function(box_, false) : Function(Closure()); }
    HOT_HIDDEN Function to_function() && { return box_ ? take() : Function(Closure()); }

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
    // Moves the guarded lambda out of the box and frees the box. The moved pin has generation 0,
    // so the pin count does not change.
    HOT_HIDDEN Function take()
    {
        Function f = box_->ops->to_function(box_, true);
        box_->ops->destroy(box_);
        box_ = nullptr;
        gen_ = 0;
        return f;
    }

    detail::GuardedBox<R, A...>* box_ = nullptr; // a detail::GuardedBoxOf, or null
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

// The guard of a lambda in a .cpp script (cpp_frontend.cpp). The lambda body is code of one
// generation. It reads the fields with the layout of that generation, and it reads the module
// block of that generation. After a reload, the state has a new layout and the old module block
// is free. So the call checks the live flag of the module first. When the flag is 0, the call
// logs a stale call and returns R() (nothing for void). It does not run the body. A reference
// result, or a type with no default value, throws ScriptError: no value is available.
namespace detail
{
[[noreturn]] HOT_HIDDEN void throw_stale_lambda(const char* what)
{
    throw ScriptError(std::string("stale lambda: ") + what +
        " is code of an old generation and has no value to give; make the lambda again after the reload");
}

template <class F> struct CppLambda
{
    const uint32_t* live;
    const char* what;
    mutable F fn;

    template <class... A> HOT_HIDDEN static decltype(auto) run(const CppLambda& self, A&&... a)
    {
        using R = decltype(self.fn(std::forward<A>(a)...));
        if (HOT_UNLIKELY(!*self.live))
        {
            stale_call(self.what);
            if constexpr (std::is_void_v<R>)
                return;
            else if constexpr (!std::is_reference_v<R> && std::is_default_constructible_v<R>)
                return R();
            else
                throw_stale_lambda(self.what);
        }
        return self.fn(std::forward<A>(a)...);
    }
    template <class... A> HOT_HIDDEN decltype(auto) operator()(A&&... a) const
    {
        return run(*this, std::forward<A>(a)...);
    }

    // A function pointer cannot check the live flag. So the conversion is an error with a clear
    // text. A lambda that uses no field and no module variable has no guard and converts.
    template <class P, std::enable_if_t<std::is_pointer_v<P> && std::is_function_v<std::remove_pointer_t<P>>, int> = 0>
    operator P() const
    {
        static_assert(sizeof(P) == 0,
            "a lambda in a .cpp script that uses a field or a module variable cannot become a function pointer: "
            "a reload can make its code stale; use std::function");
        return nullptr;
    }
};
} // namespace detail

// The translator wraps each .cpp lambda that uses the object or a module variable in this call.
template <class F> HOT_HIDDEN detail::CppLambda<F> cpp_lambda(const uint32_t* live, const char* what, F fn)
{
    return detail::CppLambda<F>{live, what, std::move(fn)};
}

} // namespace hot
