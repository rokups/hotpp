#include "hot/runtime/state.h"

#include "runtime_internal.h"

#include <cassert>

namespace hot
{

LiveObjects g_live;

namespace
{
#ifndef NDEBUG
// The debug check of the thread rule: only one thread at a time changes the list. The flag uses
// relaxed operations, so it gives no order between threads and TSan still sees a race.
std::atomic<bool> g_live_busy{false};
#endif
struct ChangeCheck
{
#ifndef NDEBUG
    ChangeCheck()
    {
        [[maybe_unused]] const bool busy = g_live_busy.exchange(true, std::memory_order_relaxed);
        assert(!busy && "two threads create or destroy script objects at the same time");
    }
    ~ChangeCheck() { g_live_busy.store(false, std::memory_order_relaxed); }
#endif
};
} // namespace

void LiveObjects::insert(ShellBase* s)
{
    ChangeCheck check;
    s->live_prev = tail_;
    s->live_next = nullptr;
    if (tail_)
        tail_->live_next = s;
    else
        head_ = s;
    tail_ = s;
    ++size_;
}

void LiveObjects::erase(ShellBase* s)
{
    ChangeCheck check;
    if (s->live_prev)
        s->live_prev->live_next = s->live_next;
    else if (head_ == s)
        head_ = s->live_next;
    else
        return; // not in the list
    if (s->live_next)
        s->live_next->live_prev = s->live_prev;
    else
        tail_ = s->live_prev;
    s->live_prev = nullptr;
    s->live_next = nullptr;
    --size_;
}

void track_object(ShellBase* self)
{
    g_live.insert(self);
}
void untrack_object(ShellBase* self)
{
    g_live.erase(self);
}

} // namespace hot
