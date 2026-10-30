// Engine API for the minengine harness (design section 8.1).
// Script code calls these members directly. There is no wrapper code.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The constructors let a script write "Vec3(x, y, z)" in C++17. C++17 has no "()" form for an aggregate.
struct Vec3 {
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
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
    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    ~World();

    void Add(Actor* actor);
    void Tick(float dt);

    // Like Tick, but the actors tick on "threads" worker threads. The workers
    // start once and stay until the world ends. Each worker ticks one chunk of
    // the actor list. The calling thread waits until all workers finished the
    // chunks. Then it runs the timers and the frame boundary hook. The workers
    // are idle at that time, so the hook is a safe point for a reload commit.
    // Do not add or remove an actor while this function runs.
    void TickParallel(float dt, unsigned threads);

    // The observer runs after each actor tick, on the thread that ticked the
    // actor. The test code uses it to check the state of the actor.
    void SetTickObserver(std::function<void(Actor*)> observer) { observer_ = std::move(observer); }
    const std::vector<Actor*>& Actors() const { return actors_; }

    // The hook runs at the end of each Tick, when no actor runs. It is the
    // safe point for a script reload commit.
    void SetFrameBoundaryHook(std::function<void()> hook) { hook_ = std::move(hook); }

    // A one-shot timer. Tick fires it when "seconds" of Tick time passed, and
    // then destroys the callable. It runs before the frame boundary hook.
    void AddTimer(float seconds, std::function<void()> fn);
    void ClearTimers() { timers_.clear(); }
    size_t TimerCount() const { return timers_.size(); }

    // Full reload support. Save writes one line for each actor: "x y z name". Clear deletes
    // every actor and drops the timers and the tick observer, which can hold script closures.
    // The frame boundary hook stays: it is host code. Restore makes the actors again.
    // "make" gives a new actor, a script object. Call Clear before the Loader reset.
    std::string Save() const;
    void Clear();
    void Restore(const std::string& text, const std::function<Actor*()>& make);

private:
    struct Pool;  // the worker threads and the barrier
    void TickActor(Actor* actor, float dt);
    void EndFrame(float dt);  // timers, then the frame boundary hook

    struct Timer {
        float remaining;
        std::function<void()> fn;
    };
    std::vector<Actor*> actors_;
    std::vector<Timer> timers_;
    std::function<void()> hook_;
    std::function<void(Actor*)> observer_;
    std::unique_ptr<Pool> pool_;
};
