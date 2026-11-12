// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/runtime/lambda.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hot
{

namespace
{
// The runtime list of the boxes. A box leaves the list when its last reference goes.
struct BoxList
{
    std::mutex lock;
    LambdaBox* head = nullptr;
    size_t size = 0;
};

BoxList& boxes()
{
    static BoxList list;
    return list;
}
} // namespace

LambdaBox* lambda_box_new(LambdaRecord* rec, void* entry, void* caps, void (*destroy)(void*))
{
    LambdaBox* box = new LambdaBox;
    box->entry.store(entry, std::memory_order_relaxed);
    box->caps = caps;
    box->rec = rec;
    box->destroy = destroy;
    BoxList& l = boxes();
    std::lock_guard<std::mutex> g(l.lock);
    box->next = l.head;
    if (l.head)
        l.head->prev = box;
    l.head = box;
    ++l.size;
    return box;
}

void lambda_box_detach(LambdaBox* box)
{
    // The exchange makes sure that only one caller destroys the captures.
    if (box->entry.exchange(nullptr, std::memory_order_acq_rel))
    {
        box->destroy(box->caps);
        box->caps = nullptr;
    }
}

void lambda_box_release(LambdaBox* box)
{
    // The entry comes before the count. So the last release and its destructors run before a
    // commit window or after it, never while a commit changes the box (EntryCounters mode).
    ScriptEntry entry;
    if (box->refs.fetch_sub(1, std::memory_order_acq_rel) != 1)
        return;
    lambda_box_detach(box);
    {
        BoxList& l = boxes();
        std::lock_guard<std::mutex> g(l.lock);
        if (box->prev)
            box->prev->next = box->next;
        else
            l.head = box->next;
        if (box->next)
            box->next->prev = box->prev;
        --l.size;
    }
    delete box;
}

size_t lambda_box_count()
{
    BoxList& l = boxes();
    std::lock_guard<std::mutex> g(l.lock);
    return l.size;
}

namespace
{
// Takes a reference to each box of the list that "want" selects. A box with no reference is in
// lambda_box_release on another thread: that thread removes it, so the function skips it.
// The caller releases each box of the result after it releases the list lock.
template <class Want> std::vector<LambdaBox*> take_boxes(Want want)
{
    std::vector<LambdaBox*> taken;
    BoxList& l = boxes();
    std::lock_guard<std::mutex> g(l.lock);
    taken.reserve(l.size);
    for (LambdaBox* b = l.head; b; b = b->next)
    {
        if (!want(b))
            continue;
        uint32_t n = b->refs.load(std::memory_order_relaxed);
        while (n != 0 && !b->refs.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel))
        {
        }
        if (n != 0)
            taken.push_back(b);
    }
    return taken;
}
} // namespace

void detail::for_each_lambda_box_impl(void (*fn)(LambdaBox*, void*), void* ctx)
{
    // The destructor of a capture can release another box, and so take the list lock. So "fn"
    // runs without the lock.
    for (LambdaBox* b : take_boxes([](LambdaBox*) { return true; }))
    {
        fn(b, ctx);
        lambda_box_release(b);
    }
}

size_t lambda_detach_module(const uint32_t* live)
{
    if (!live)
        return 0;
    // Only the loader thread changes "rec". It reads the record while the module is loaded.
    std::vector<LambdaBox*> taken = take_boxes([live](LambdaBox* b) { return b->rec && b->rec->live == live; });
    for (LambdaBox* b : taken)
    {
        b->rec = nullptr;
        lambda_box_detach(b);
        lambda_box_release(b);
    }
    return taken.size();
}

namespace
{
// The plan of one old record: the new record and the old index of each new capture. A null "to"
// detaches the boxes, and "cause" tells why.
struct RebindPlan
{
    LambdaRecord* to = nullptr;
    std::vector<uint32_t> from;
    std::string cause;
};

bool same_text(const char* a, const char* b)
{
    return a == b || (a && b && std::strcmp(a, b) == 0);
}

RebindPlan make_plan(const LambdaRecord& o, const LambdaRecord* found)
{
    RebindPlan p;
    if (!found)
    {
        p.cause = "the lambda is removed";
        return p;
    }
    LambdaRecord* n = found->self ? found->self : const_cast<LambdaRecord*>(found);
    if (!o.sig || !n->sig || std::strcmp(o.sig, n->sig) != 0)
    {
        p.cause = "the signature changed";
        return p;
    }
    if ((o.count && (!o.names || !o.types || !o.field)) || (n->count && (!n->names || !n->types)) || !n->build)
    {
        p.cause = "the captures have no layout";
        return p;
    }
    const bool layout_changed = !same_text(o.layout, n->layout);
    for (uint32_t i = 0; i < n->count; ++i)
    {
        uint32_t j = 0;
        while (j < o.count && std::strcmp(o.names[j], n->names[i]) != 0)
            ++j;
        if (j == o.count)
        {
            p.cause = std::string("capture ") + n->names[i] + " is added";
            return p;
        }
        if (std::strcmp(o.types[j], n->types[i]) != 0)
        {
            p.cause = std::string("capture ") + n->names[i] + " changed its type";
            return p;
        }
        if (layout_changed && ((n->flags && (n->flags[i] & kCaptureLayoutBound)) || !n->flags))
        {
            p.cause = std::string("capture ") + n->names[i] + " depends on a script struct layout that changed";
            return p;
        }
        p.from.push_back(j);
    }
    p.to = n;
    return p;
}

// The entry is null at once. The capture block goes to "defer", or it dies now.
void detach_box(LambdaBox* b, CaptureGraveyard* defer)
{
    b->rec = nullptr;
    if (!defer)
    {
        lambda_box_detach(b);
        return;
    }
    if (b->entry.exchange(nullptr, std::memory_order_acq_rel))
    {
        defer->add_caps(b->caps, b->destroy);
        b->caps = nullptr;
    }
}
} // namespace

void CaptureGraveyard::run()
{
    // A destructor can add more work (a closure that releases another box).
    while (!caps_.empty() || !boxes_.empty())
    {
        auto caps = std::move(caps_);
        auto boxes = std::move(boxes_);
        caps_.clear();
        boxes_.clear();
        for (const auto& [c, d] : caps)
            d(c);
        for (LambdaBox* b : boxes)
            lambda_box_release(b);
    }
}

LambdaRebind lambda_rebind_module(const uint32_t* live, LambdaFind find, void* ctx, CaptureGraveyard* defer)
{
    LambdaRebind result;
    if (!live)
        return result;
    // Only the loader thread changes "rec". The commit window excludes each call of a box, and
    // the reference of the list keeps each box alive. The work runs after the list lock goes:
    // a capture destructor can release another box.
    std::vector<LambdaBox*> taken = take_boxes([live](LambdaBox* b) { return b->rec && b->rec->live == live; });
    std::unordered_map<const LambdaRecord*, RebindPlan> plans;
    std::vector<std::pair<std::string, std::string>> order; // the key and the cause of each warning
    std::unordered_map<std::string, size_t> detached_by_key;
    std::vector<void*> src;
    for (LambdaBox* b : taken)
    {
        const LambdaRecord* o = b->rec;
        auto it = plans.find(o);
        if (it == plans.end())
            it = plans.emplace(o, make_plan(*o, find(o->key, ctx))).first;
        const RebindPlan& p = it->second;
        std::string cause = p.cause;
        void* caps = nullptr;
        if (p.to && b->entry.load(std::memory_order_acquire))
        {
            src.assign(p.from.size(), nullptr);
            for (size_t i = 0; i < p.from.size(); ++i)
                src[i] = o->field(b->caps, p.from[i]);
            try
            {
                caps = p.to->build(src.data());
            }
            catch (...)
            {
                cause = "a capture move threw";
            }
        }
        if (caps)
        {
            void* old = b->caps;
            void (*old_destroy)(void*) = b->destroy;
            b->caps = caps;
            b->destroy = p.to->destroy;
            b->rec = p.to;
            b->entry.store(p.to->entry, std::memory_order_release);
            // The moved-from values and the removed captures.
            if (defer)
                defer->add_caps(old, old_destroy);
            else
                old_destroy(old);
            ++result.rebound;
        }
        else
        {
            const bool live_box = b->entry.load(std::memory_order_acquire) != nullptr;
            detach_box(b, defer);
            if (live_box)
            {
                ++result.detached;
                if (detached_by_key[o->key]++ == 0)
                    order.emplace_back(o->key, cause);
            }
        }
        if (defer)
            defer->add_box(b);
        else
            lambda_box_release(b);
    }
    for (const auto& [key, cause] : order)
        std::fprintf(stderr, "[script] WARNING: lambda %s: %s; %zu stored closures are detached\n", key.c_str(),
            cause.c_str(), detached_by_key[key]);
    return result;
}

} // namespace hot
