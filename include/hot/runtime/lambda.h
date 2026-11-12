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
    void* entry = nullptr;             // R (*)(void* caps, P...)
    void (*destroy)(void*) = nullptr;  // destroys and frees a capture block of this lambda
};

// The heap block of one lambda value. The runtime owns it. Closures share it with a count.
// "entry" is null when the box is detached: the captures are destroyed and a call is stale.
struct LambdaBox
{
    std::atomic<uint32_t> refs{1};
    std::atomic<void*> entry{nullptr};
    void* caps = nullptr;
    LambdaRecord* rec = nullptr;      // the record of the module that made the box
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
// The count of the boxes in the runtime list.
HOT_EXPORT size_t lambda_box_count();

namespace detail
{
// The list holds its lock while it calls "fn". "fn" can call lambda_box_detach, but it must not
// make or release a box.
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

// The entry of a lambda. The lambda type has no capture, so a new object of it is the lambda.
// Before C++20 a lambda type has no default constructor. Then the entry uses an empty object
// that has the size and the alignment of F. The call reads no byte of it.
template <class Caps, class F, class R, class... P> R lambda_entry(void* caps, P... p)
{
    static_assert(std::is_empty<F>::value, "the lambda of the lowering has no capture");
    if constexpr (std::is_default_constructible<F>::value)
        return F{}(*static_cast<Caps*>(caps), static_cast<P&&>(p)...);
    else
    {
        alignas(F) static const unsigned char empty[1] = {};
        return (*reinterpret_cast<const F*>(empty))(*static_cast<Caps*>(caps), static_cast<P&&>(p)...);
    }
}

template <class Caps> void lambda_destroy_caps(void* caps)
{
    delete static_cast<Caps*>(caps);
}

template <class Tag, class Sig, class Caps, class F> struct LambdaReg;
template <class Tag, class R, class... P, class Caps, class F> struct LambdaReg<Tag, R(P...), Caps, F>
{
    static LambdaRecord make()
    {
        return {Tag::key, typeid(R(P...)).name(), reinterpret_cast<void*>(&lambda_entry<Caps, F, R, P...>),
            &lambda_destroy_caps<Caps>};
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
            stale_call("lambda");
            return R();
        }
        return reinterpret_cast<R (*)(void*, A...)>(fn)(box_->caps, std::forward<A>(a)...);
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
    return LambdaClosure<Sig>(box, gen);
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
