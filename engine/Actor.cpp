// Out-of-line definitions keep the vtable and the type_info of Actor in the
// executable only. A shared library then uses the executable symbols.
#include "Actor.h"

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

#include "script_runtime.h"

Actor::Actor() : Name("Actor") {}
Actor::Actor(std::string name) : Name(std::move(name)) {}
Actor::~Actor() = default;

void Actor::Tick(float dt)
{
    std::printf("[engine] %s: Actor::Tick dt=%.4f\n", Name.c_str(), dt);
}

void Actor::TakeDamage(float amount)
{
    std::printf("[engine] %s: Actor::TakeDamage %.1f\n", Name.c_str(), amount);
}

void Actor::TakeDamage(Actor& source, float amount)
{
    std::printf("[engine] %s: Actor::TakeDamage %.1f from %s\n", Name.c_str(), amount,
                source.Name.c_str());
}

void Actor::Move(Vec3 delta)
{
    Position.x += delta.x;
    Position.y += delta.y;
    Position.z += delta.z;
}

World::World() = default;

World::~World()
{
    pool_.reset();  // join the workers before the actors go
    for (Actor* a : actors_) delete a;
}

// The worker threads of TickParallel.
// A worker waits for a new frame (epoch). It ticks its chunk and then reports.
// The caller of run() waits until all workers reported.
// The mutex orders the memory. After run(), the caller sees all writes of the workers.
// In the next frame, the workers see all writes of the caller.
// A worker is outside script code while it waits.
struct World::Pool {
    World* world;
    std::mutex mutex;
    std::condition_variable start_cv, done_cv;
    std::vector<std::thread> threads;
    uint64_t epoch = 0;
    unsigned running = 0;  // workers that did not report in this frame
    bool stop = false;
    float dt = 0;

    Pool(World* w, unsigned count) : world(w)
    {
        for (unsigned i = 0; i < count; ++i) threads.emplace_back([this, i, count] { work(i, count); });
    }

    ~Pool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        start_cv.notify_all();
        for (std::thread& t : threads) t.join();
    }

    void work(unsigned index, unsigned count)
    {
        uint64_t seen = 0;
        for (;;) {
            float frame_dt;
            {
                std::unique_lock<std::mutex> lock(mutex);
                start_cv.wait(lock, [&] { return stop || epoch != seen; });
                if (stop) return;
                seen = epoch;
                frame_dt = dt;
            }
            const size_t n = world->actors_.size();
            for (size_t i = n * index / count; i < n * (index + 1) / count; ++i)
                world->TickActor(world->actors_[i], frame_dt);
            std::lock_guard<std::mutex> lock(mutex);
            if (--running == 0) done_cv.notify_one();
        }
    }

    // The barrier: returns when all workers ticked their chunks.
    void run(float frame_dt)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            dt = frame_dt;
            running = static_cast<unsigned>(threads.size());
            ++epoch;
        }
        start_cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        done_cv.wait(lock, [&] { return running == 0; });
    }
};

void World::Add(Actor* actor)
{
    actors_.push_back(actor);
}

void World::AddTimer(float seconds, std::function<void()> fn)
{
    timers_.push_back({seconds, std::move(fn)});
}

void World::TickActor(Actor* actor, float dt)
{
    script::ScriptTickScope scope;  // the debug check of the safe point
    actor->Tick(dt);
    if (observer_) observer_(actor);
}

void World::Tick(float dt)
{
    for (Actor* a : actors_) TickActor(a, dt);
    EndFrame(dt);
}

void World::TickParallel(float dt, unsigned threads)
{
    if (threads == 0) threads = 1;
    if (!pool_ || pool_->threads.size() != threads) pool_ = std::make_unique<Pool>(this, threads);
    pool_->run(dt);
    EndFrame(dt);
}

void World::EndFrame(float dt)
{    // Move the due timers out first: a callable can add a timer.
    std::vector<Timer> due;
    for (size_t i = 0; i < timers_.size();) {
        timers_[i].remaining -= dt;
        if (timers_[i].remaining <= 0.0f) {
            due.push_back(std::move(timers_[i]));
            timers_.erase(timers_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (Timer& t : due) {
        if (t.fn) t.fn();
        t.fn = nullptr;  // the callable ends here, before the hook
    }
    if (hook_) hook_();  // frame boundary
}
