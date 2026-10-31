#include "hot/runtime/state.h"

#include "runtime_internal.h"

namespace hot
{

LiveObjects g_live;

void LiveObjects::insert(ShellBase* s)
{
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
