// Data that the files of the runtime share. This header is not public.
#pragma once
#include "hot/runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hot
{

// The live objects (state.cpp): a list through the members live_prev and live_next of each
// shell, in the order of creation. A walk gives the pair (shell, creator). The creator is the
// generation whose code made the object with "new" (construct_shell), or 0. That code also made
// the handle of the object, for example the control block of a std::shared_ptr. So the
// generation must stay loaded while the object lives. Do not insert or erase during a walk.
// The list has no lock: a lock costs about 3 ns for each "new" and free. Only one thread at a
// time creates or destroys script objects (doc/internals.md, "Threads and the safe point"). A
// debug build asserts when two changes overlap. The reload walks the list on the main thread at
// the safe point, when no other thread creates or destroys an object.
class LiveObjects
{
public:
    class iterator
    {
    public:
        explicit iterator(ShellBase* s) : s_(s) {}
        std::pair<ShellBase*, uint32_t> operator*() const { return {s_, s_->creator}; }
        iterator& operator++()
        {
            s_ = s_->live_next;
            return *this;
        }
        bool operator!=(const iterator& o) const { return s_ != o.s_; }
        bool operator==(const iterator& o) const { return s_ == o.s_; }

    private:
        ShellBase* s_;
    };

    void insert(ShellBase* s);
    void erase(ShellBase* s); // a shell that is not in the list is ignored
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    iterator begin() const { return iterator(head_); }
    iterator end() const { return iterator(nullptr); }

private:
    ShellBase* head_ = nullptr;
    ShellBase* tail_ = nullptr;
    size_t size_ = 0;
};

extern LiveObjects g_live;

// The count of commits that ran while a thread was in script code (threads.cpp).
extern std::atomic<uint64_t> g_unsafe_commits;

// Fills a new state block (migration.cpp).
void migrate_block(const StateLayout& o, const StateLayout& n, const MigrationPlan& plan, char* from, char* to);

} // namespace hot
