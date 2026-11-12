// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Handler holders in the style of the engine. They have the same overload forms as the engine
// templates, so a script method group compiles against the real forms:
//  - Signal<Args...>::Subscribe(receiver, handler), as Urho3D::Signal::Subscribe. The handler takes
//    (Receiver*, Args...) or (Args...). It returns void or bool.
//  - EventHub::SubscribeToEvent(eventType, handler), as Urho3D::Object::SubscribeToEvent. The
//    handler takes (EventHub*, int, float), (int, float), (float) or nothing.
// The holders store the wrapped handlers in std::function objects.
#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

template <class... Args> class Signal
{
public:
    template <class Receiver, class Callback> void Subscribe(Receiver* receiver, Callback handler)
    {
        static_assert(std::is_invocable_v<Callback, Receiver*, Args...> || std::is_invocable_v<Callback, Args...>,
            "Callback should accept either (Args...) or (Receiver*, Args...) as parameters.");
        subs_.push_back({receiver,
            [handler](void* r, Args... args) mutable
            {
                auto* receiverPtr = static_cast<Receiver*>(r);
                if constexpr (std::is_invocable_v<Callback, Receiver*, Args...>)
                    std::invoke(handler, receiverPtr, args...);
                else
                    std::invoke(handler, args...);
            }});
    }

    void operator()(Args... args)
    {
        for (Sub& s : subs_)
            s.fn(s.receiver, args...);
    }

    size_t Count() const { return subs_.size(); }
    void Clear() { subs_.clear(); }

private:
    struct Sub
    {
        void* receiver;
        std::function<void(void*, Args...)> fn;
    };
    std::vector<Sub> subs_;
};

class EventHub
{
public:
    template <class T> void SubscribeToEvent(int eventType, T handler)
    {
        constexpr bool hasObjectTypeData = std::is_invocable_r_v<void, T, EventHub*, int, float>;
        constexpr bool hasTypeData = std::is_invocable_r_v<void, T, int, float>;
        constexpr bool hasData = std::is_invocable_r_v<void, T, float>;
        constexpr bool hasNone = std::is_invocable_r_v<void, T>;
        static_assert(hasObjectTypeData || hasTypeData || hasData || hasNone, "Invalid handler signature");
        std::function<void(EventHub*, int, float)> fn;
        if constexpr (hasObjectTypeData)
            fn = [handler](EventHub* r, int t, float v) mutable { handler(r, t, v); };
        else if constexpr (hasTypeData)
            fn = [handler](EventHub*, int t, float v) mutable { handler(t, v); };
        else if constexpr (hasData)
            fn = [handler](EventHub*, int, float v) mutable { handler(v); };
        else
            fn = [handler](EventHub*, int, float) mutable { handler(); };
        handlers_.push_back({eventType, std::move(fn)});
    }

    void SendEvent(int eventType, float value)
    {
        for (Handler& h : handlers_)
            if (h.type == eventType)
                h.fn(this, eventType, value);
    }

    size_t Count() const { return handlers_.size(); }
    void Clear() { handlers_.clear(); }

private:
    struct Handler
    {
        int type;
        std::function<void(EventHub*, int, float)> fn;
    };
    std::vector<Handler> handlers_;
};
