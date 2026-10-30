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
// unverified: not built on MSVC. The MSVC form is the plain condition.
    #define HOT_UNLIKELY(x) (x)
#endif

namespace hot
{

// A closure that leaves script code (section 4.11). It holds one pin of its
// generation for its whole life. Each call checks the generation guard: when the
// generation is not live, the call logs a stale call and returns R(). The lambda
// body does not run. A copy of the closure holds its own pin.
//
// The closure converts to std::function (the constructor of std::function takes
// any callable) and to an engine delegate type D that has Bind(std::function<Sig>).
// The closure code lives in the generation module. Destroy the last copy on the
// main thread, before the frame boundary at which the generation can unload.
template <class Sig> class Closure;

template <class R, class... A> class Closure<R(A...)>
{
public:
    using Function = std::function<R(A...)>;

    HOT_HIDDEN Closure() {}

    template <class F>
    HOT_HIDDEN Closure(uint32_t generation, const char* what, F fn)
        : fn_(std::move(fn))
        , gen_(generation)
        , what_(what)
    {
        pin(gen_);
    }

    HOT_HIDDEN Closure(const Closure& o)
        : fn_(o.fn_)
        , gen_(o.gen_)
        , what_(o.what_)
    {
        if (gen_)
            pin(gen_);
    }

    HOT_HIDDEN Closure(Closure&& o) noexcept
        : fn_(std::move(o.fn_))
        , gen_(o.gen_)
        , what_(o.what_)
    {
        o.fn_ = nullptr;
        o.gen_ = 0;
    }

    HOT_HIDDEN Closure& operator=(Closure o) noexcept
    {
        std::swap(fn_, o.fn_);
        std::swap(gen_, o.gen_);
        std::swap(what_, o.what_);
        return *this;
    }

    // The lambda goes first (its captures can use the generation code), then the pin.
    HOT_HIDDEN ~Closure()
    {
        fn_ = nullptr;
        if (gen_)
            unpin(gen_);
    }

    HOT_HIDDEN R operator()(A... a) const
    {
        ScriptEntry entry; // design 6.3: the guard reads active_generation
        if (HOT_UNLIKELY(!fn_ || gen_ != active_generation))
        {
            stale_call(what_ ? what_ : "closure");
            return R();
        }
        return fn_(std::forward<A>(a)...);
    }

    HOT_HIDDEN explicit operator bool() const { return gen_ != 0; }
    HOT_HIDDEN uint32_t generation() const { return gen_; }

    // A std::function that holds a copy of this closure (and so one more pin).
    HOT_HIDDEN Function to_function() const
    {
        Closure copy(*this);
        return Function(std::move(copy));
    }

    // Conversion to an engine delegate type that binds a std::function.
    template <class D, std::enable_if_t<detail::has_bind<D, Function>::value, int> = 0> HOT_HIDDEN operator D() const
    {
        D d;
        d.Bind(to_function());
        return d;
    }

private:
    Function fn_;
    uint32_t gen_ = 0;
    const char* what_ = nullptr;
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

} // namespace hot
