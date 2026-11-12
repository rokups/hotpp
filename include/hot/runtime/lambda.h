// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The registration of a reloadable lambda at module load, and the box closure of a lambda (spike).
//
// The lowering of a lambda gives a tag struct with the static member "key", and a call of
// make_lambda<Tag, Sig>(gen, caps, f). The call instantiates LambdaReg<Tag, Sig, Caps, F>. A static
// member of this class template adds the record to a list of the module when the module loads.
// This happens also when the lambda never runs. script_register gives the list to the registry
// with flush_lambdas.
//
// The call also makes a LambdaBox on the heap. The box holds the capture block and the entry
// function. The box is in a runtime list, so a later commit can find each box by its record and
// detach it. LambdaClosure is the value of the lambda: a box pointer and a generation.
#pragma once
#include "hot/runtime/closure.h"
#include "hot/runtime/compat.h"
#include "hot/runtime/hidden.h"
#include "hot/runtime/threads.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace hot
{

// The registration of one lambda. The strings and the arrays have static storage in the module.
// "sig" is typeid(Sig).name(). "names" has the names of the captures in the order of the capture
// tuple. "aliases" has the old keys of the lambda. Each array ends with a null pointer. "live" is
// the live flag of the module that holds the code (null: no flag).
struct LambdaRecord
{
    const char* key = nullptr;
    const char* sig = nullptr;
    void* entry = nullptr;             // R (*)(void* caps, P...)
    void (*destroy)(void*) = nullptr;  // destroys and frees a capture block of this lambda
    const char* what = nullptr;        // the name of the lambda in the stale call log
    const char* const* names = nullptr;
    const char* const* aliases = nullptr;
    const uint32_t* live = nullptr;
};

// The heap block of one lambda value. The runtime owns it. Closures share it with a count.
// "entry" is null when the box is detached: the captures are destroyed and a call is stale.
struct LambdaBox
{
    std::atomic<uint32_t> refs{1};
    std::atomic<void*> entry{nullptr};
    void* caps = nullptr;
    LambdaRecord* rec = nullptr;      // the record of the module that made the box; null when detached
    const char* what = nullptr;       // the name in the stale call log (module memory, see LambdaClosure)
    void (*destroy)(void*) = nullptr; // the destroy function of the capture block
    LambdaBox* prev = nullptr;        // the links of the runtime box list
    LambdaBox* next = nullptr;
};

// Makes a box with one reference and adds it to the runtime box list.
HOT_NODEBUG HOT_EXPORT LambdaBox* lambda_box_new(LambdaRecord* rec, void* entry, void* caps, void (*destroy)(void*));
// Removes one reference. The last one destroys the captures (if not detached), removes the box
// from the list and frees it.
HOT_NODEBUG HOT_EXPORT void lambda_box_release(LambdaBox* box);
// Destroys the captures and sets the entry to null. A second call does nothing.
HOT_NODEBUG HOT_EXPORT void lambda_box_detach(LambdaBox* box);
// Detaches each box whose record has the live flag "live" and sets the record of the box to null.
// The loader calls it when the module of the flag retires, before the module unloads. A capture
// can hold a closure of another box: the destructors run after the list lock is released.
// Returns the count of the boxes that it detached.
HOT_EXPORT size_t lambda_detach_module(const uint32_t* live);
// The count of the boxes in the runtime list.
HOT_EXPORT size_t lambda_box_count();

namespace detail
{
// The function takes a reference to each live box under the list lock. Then it releases the lock
// and calls "fn" for each box. So "fn" can detach, make and release boxes.
HOT_EXPORT void for_each_lambda_box_impl(void (*fn)(LambdaBox*, void*), void* ctx);
} // namespace detail

// Calls fn(LambdaBox*, const LambdaRecord*) for each live box.
template <class Fn> void for_each_lambda_box(Fn&& fn)
{
    detail::for_each_lambda_box_impl(
        [](LambdaBox* box, void* ctx) { (*static_cast<std::remove_reference_t<Fn>*>(ctx))(box, box->rec); }, &fn);
}

namespace detail
{
// The lambdas that the module registered since the last flush. Each module has its own list: the
// function is inline and the module is built with hidden visibility.
inline std::vector<LambdaRecord>& pending_lambdas()
{
    static std::vector<LambdaRecord> list;
    return list;
}

// The lambda type has no capture, so a new object of it is the lambda. Before C++20 a lambda type
// has no default constructor. Then the function gives an empty object that has the size and the
// alignment of F. The call reads no byte of it.
template <class F> const F& lambda_object()
{
    if constexpr (std::is_default_constructible<F>::value)
    {
        static const F f{};
        return f;
    }
    else
    {
        alignas(F) static const unsigned char empty[1] = {};
        return *reinterpret_cast<const F*>(empty);
    }
}

// The entry of a lambda.
template <class Caps, class F, class R, class... P> R lambda_entry(void* caps, P... p)
{
    static_assert(std::is_empty<F>::value, "the lambda of the lowering has no capture");
    // A void signature drops the value of the body (a std::function<void()> target, for example).
    if constexpr (std::is_void<R>::value)
        lambda_object<F>()(*static_cast<Caps*>(caps), static_cast<P&&>(p)...);
    else
        return lambda_object<F>()(*static_cast<Caps*>(caps), static_cast<P&&>(p)...);
}

// The optional members of a tag: "what", "names", "aliases" and "live()".
template <class T, class = void> struct tag_what
{
    static constexpr const char* get() { return nullptr; }
};
template <class T> struct tag_what<T, std::void_t<decltype(T::what)>>
{
    static constexpr const char* get() { return T::what; }
};
template <class T, class = void> struct tag_names
{
    static constexpr const char* const* get() { return nullptr; }
};
template <class T> struct tag_names<T, std::void_t<decltype(T::names)>>
{
    static constexpr const char* const* get() { return T::names; }
};
template <class T, class = void> struct tag_aliases
{
    static constexpr const char* const* get() { return nullptr; }
};
template <class T> struct tag_aliases<T, std::void_t<decltype(T::aliases)>>
{
    static constexpr const char* const* get() { return T::aliases; }
};
template <class T, class = void> struct tag_live
{
    static const uint32_t* get() { return nullptr; }
};
template <class T> struct tag_live<T, std::void_t<decltype(T::live())>>
{
    static const uint32_t* get() { return T::live(); }
};

template <class Caps> void lambda_destroy_caps(void* caps)
{
    delete static_cast<Caps*>(caps);
}

template <class Tag, class Sig, class Caps, class F> struct LambdaReg;
template <class Tag, class R, class... P, class Caps, class F> struct LambdaReg<Tag, R(P...), Caps, F>
{
    static LambdaRecord make()
    {
        LambdaRecord r;
        r.key = Tag::key;
        r.sig = typeid(R(P...)).name();
        r.entry = reinterpret_cast<void*>(&lambda_entry<Caps, F, R, P...>);
        r.destroy = &lambda_destroy_caps<Caps>;
        r.what = tag_what<Tag>::get();
        r.names = tag_names<Tag>::get();
        r.aliases = tag_aliases<Tag>::get();
        r.live = tag_live<Tag>::get();
        return r;
    }
    // The record of the module. The boxes point to it. It has static storage.
    static LambdaRecord& record()
    {
        static LambdaRecord r = make();
        return r;
    }
    static bool add()
    {
        pending_lambdas().push_back(make());
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

// The value of a reloadable lambda: a box pointer and the generation of the code. A copy adds a
// box reference and pins the generation. A move gives the box and the pin to the new closure.
// The destructor releases the box, then the pin. A call checks the entry of the box. A null entry
// is a detached box: the call logs a stale call and returns R().
template <class Sig> class LambdaClosure;

template <class R, class... A> class LambdaClosure<R(A...)>
{
public:
    HOT_HIDDEN LambdaClosure() {}

    // Takes the reference of the box that lambda_box_new made.
    HOT_HIDDEN LambdaClosure(LambdaBox* adopt, uint32_t generation)
        : box_(adopt)
        , gen_(generation)
    {
        if (gen_)
            pin(gen_);
    }

    HOT_HIDDEN LambdaClosure(const LambdaClosure& o)
        : box_(o.box_)
        , gen_(o.gen_)
    {
        if (box_)
            box_->refs.fetch_add(1, std::memory_order_relaxed);
        if (gen_)
            pin(gen_);
    }

    HOT_HIDDEN LambdaClosure(LambdaClosure&& o) noexcept
        : box_(o.box_)
        , gen_(o.gen_)
    {
        o.box_ = nullptr;
        o.gen_ = 0;
    }

    HOT_HIDDEN LambdaClosure& operator=(LambdaClosure o) noexcept
    {
        std::swap(box_, o.box_);
        std::swap(gen_, o.gen_);
        return *this;
    }

    HOT_HIDDEN ~LambdaClosure()
    {
        if (box_)
            lambda_box_release(box_);
        if (gen_)
            unpin(gen_);
    }

    HOT_HIDDEN R operator()(A... a) const
    {
        ScriptEntry entry;
        void* fn = box_ ? box_->entry.load(std::memory_order_acquire) : nullptr;
        if (HOT_UNLIKELY(!fn))
        {
            stale_call(box_ && box_->what ? box_->what : "lambda");
            return R();
        }
        return reinterpret_cast<R (*)(void*, A...)>(fn)(box_->caps, std::forward<A>(a)...);
    }

    // Conversion to an engine delegate type that binds a std::function. The std::function holds a
    // copy of the closure. A std::function and a template parameter of a native function take the
    // closure directly: it is a copyable function object.
    template <class D, std::enable_if_t<detail::has_bind<D, std::function<R(A...)>>::value, int> = 0>
    HOT_HIDDEN operator D() const
    {
        D d;
        d.Bind(std::function<R(A...)>(*this));
        return d;
    }

    HOT_HIDDEN explicit operator bool() const { return box_ != nullptr; }
    HOT_HIDDEN uint32_t generation() const { return gen_; }

private:
    LambdaBox* box_ = nullptr;
    uint32_t gen_ = 0;
};

// Makes the box of a lambda and registers its record. "gen" is the generation of the code.
template <class Tag, class Sig, class Caps, class F> LambdaClosure<Sig> make_lambda(uint32_t gen, Caps c, F)
{
    using Reg = detail::LambdaReg<Tag, Sig, Caps, F>;
    detail::touch(&Reg::registered);
    LambdaRecord& rec = Reg::record();
    LambdaBox* box = lambda_box_new(&rec, rec.entry, new Caps(std::move(c)), rec.destroy);
    box->what = rec.what;
    return LambdaClosure<Sig>(box, gen);
}

namespace detail
{
// The signature of the body lambda F of the lowering: "auto& c" is the first parameter, the
// other parameters have declared types. The address of operator()<Caps> gives the call type
// with the deduced result. The first parameter goes away.
template <class M> struct BodySig;
template <class C, class R, class Cap, class... P> struct BodySig<R (C::*)(Cap, P...) const>
{
    using type = R(P...);
};
template <class Caps, class F> using body_sig_t = typename BodySig<decltype(&F::template operator()<Caps>)>::type;
} // namespace detail

// The form of the translator for a lambda with declared parameter types. C++ deduces the
// signature from the body lambda: the translator knows the parameter types, but not the result
// type and not the capture types.
template <class Tag, class Caps, class F> LambdaClosure<detail::body_sig_t<Caps, F>> make_lambda_deduced(uint32_t gen, Caps c, F f)
{
    return make_lambda<Tag, detail::body_sig_t<Caps, F>>(gen, std::move(c), f);
}

// A lambda with a parameter of type "auto" has no signature of its own. The value waits for the
// target type: a conversion to LambdaClosure<Sig> or std::function<Sig> makes the box with Sig.
template <class Tag, class Caps, class F> class LambdaDeferred
{
public:
    HOT_HIDDEN LambdaDeferred(uint32_t gen, Caps c)
        : gen_(gen)
        , caps_(std::move(c))
    {
    }

    template <class R, class... A> HOT_HIDDEN operator LambdaClosure<R(A...)>() const
    {
        return make_lambda<Tag, R(A...)>(gen_, caps_, F(lambda_object()));
    }
    template <class R, class... A> HOT_HIDDEN operator std::function<R(A...)>() const
    {
        return std::function<R(A...)>(make_lambda<Tag, R(A...)>(gen_, caps_, F(lambda_object())));
    }

private:
    static const F& lambda_object() { return detail::lambda_object<F>(); }
    uint32_t gen_;
    Caps caps_;
};

// The form of the translator for a lambda with a parameter of type "auto".
template <class Tag, class Caps, class F> LambdaDeferred<Tag, Caps, F> make_lambda_deferred(uint32_t gen, Caps c, F)
{
    return LambdaDeferred<Tag, Caps, F>(gen, std::move(c));
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
