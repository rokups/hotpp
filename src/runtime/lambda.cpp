// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/runtime/lambda.h"

#include <mutex>

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

void detail::for_each_lambda_box_impl(void (*fn)(LambdaBox*, void*), void* ctx)
{
    BoxList& l = boxes();
    std::lock_guard<std::mutex> g(l.lock);
    for (LambdaBox* b = l.head; b; b = b->next)
        fn(b, ctx);
}

} // namespace hot
