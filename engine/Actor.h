// Engine API for the minengine harness (design section 8.1).
// Script code calls these members directly. There is no wrapper code.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

class Actor {
public:
    Actor();
    explicit Actor(std::string name);
    virtual ~Actor();

    virtual void Tick(float dt);
    virtual void TakeDamage(float amount);
    virtual void TakeDamage(Actor& source, float amount);

    void Move(Vec3 delta);

    std::string Name;
    Vec3 Position;
};

// A native delegate: an object and a function address. A script method is
// bound through a thunk, which keeps its address across reloads.
template <class Sig>
class Delegate;

template <class R, class... A>
class Delegate<R(A...)> {
public:
    using Fn = R (*)(Actor*, A...);

    void Bind(Actor* object, Fn fn)
    {
        object_ = object;
        fn_ = fn;
        callable_ = nullptr;
    }
    // Binds a callable, for example a script closure.
    void Bind(std::function<R(A...)> callable)
    {
        object_ = nullptr;
        fn_ = nullptr;
        callable_ = std::move(callable);
    }
    bool IsBound() const { return fn_ != nullptr || static_cast<bool>(callable_); }
    R operator()(A... a) const
    {
        if (callable_) return callable_(std::forward<A>(a)...);
        return fn_(object_, std::forward<A>(a)...);
    }

private:
    Actor* object_ = nullptr;
    Fn fn_ = nullptr;
    std::function<R(A...)> callable_;
};

// The world owns the actors that it holds. It deletes them in the destructor.
class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World();

    void Add(Actor* actor);
    void Tick(float dt);
    const std::vector<Actor*>& Actors() const { return actors_; }

    // The hook runs at the end of each Tick, when no actor runs. It is the
    // safe point for a script reload commit.
    void SetFrameBoundaryHook(std::function<void()> hook) { hook_ = std::move(hook); }

    // A one-shot timer. Tick fires it when "seconds" of Tick time passed, and
    // then destroys the callable. It runs before the frame boundary hook.
    void AddTimer(float seconds, std::function<void()> fn);
    void ClearTimers() { timers_.clear(); }
    size_t TimerCount() const { return timers_.size(); }

private:
    struct Timer {
        float remaining;
        std::function<void()> fn;
    };
    std::vector<Actor*> actors_;
    std::vector<Timer> timers_;
    std::function<void()> hook_;
};
