// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A custom intrusive smart pointer of a host, for the stub attribute "weak_handle".
// RefBase keeps a strong count and a weak block. RefPtr<T> holds a strong reference.
// WeakRef<T> does not keep the object alive. "Lock()" (not "lock()") gives a RefPtr<T>.
// The pointers convert to "T*", as the pointers of some engines do.
#pragma once
#include "hot/runtime/export.h" // HOT_RTTI

#include <cstddef>
#include <type_traits>
#include <utility>

struct HOT_RTTI RefBase
{
    struct Block
    {
        int weak = 1; // the object holds one count of its block
        bool alive = true;
    };

    RefBase() = default;
    RefBase(const RefBase&) = delete;
    RefBase& operator=(const RefBase&) = delete;
    virtual ~RefBase()
    {
        if (block_ && --block_->weak == 0)
            delete block_;
    }

    void AddRef() { ++refs_; }
    void ReleaseRef()
    {
        if (--refs_ == 0)
        {
            if (block_)
                block_->alive = false;
            delete this;
        }
    }
    int Refs() const { return refs_; }
    Block* WeakBlock()
    {
        if (!block_)
            block_ = new Block();
        return block_;
    }

private:
    int refs_ = 0;
    Block* block_ = nullptr;
};

template <class T> class RefPtr
{
public:
    RefPtr() noexcept = default;
    RefPtr(std::nullptr_t) noexcept {}
    explicit RefPtr(T* p)
        : p_(p)
    {
        if (p_)
            p_->AddRef();
    }
    RefPtr(const RefPtr& o)
        : RefPtr(o.p_)
    {
    }
    template <class U, std::enable_if_t<!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>, int> = 0>
    RefPtr(const RefPtr<U>& o)
        : RefPtr(static_cast<T*>(o.Get()))
    {
    }
    RefPtr(RefPtr&& o) noexcept
        : p_(o.p_)
    {
        o.p_ = nullptr;
    }
    RefPtr& operator=(RefPtr o) noexcept
    {
        std::swap(p_, o.p_);
        return *this;
    }
    ~RefPtr()
    {
        if (p_)
            p_->ReleaseRef();
    }

    T* Get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    operator T*() const noexcept { return p_; }

private:
    T* p_ = nullptr;
};

template <class T> class WeakRef
{
public:
    WeakRef() noexcept = default;
    WeakRef(std::nullptr_t) noexcept {}
    template <class U, std::enable_if_t<std::is_convertible_v<U*, T*>, int> = 0>
    WeakRef(const RefPtr<U>& s)
        : p_(s.Get())
        , block_(p_ ? p_->WeakBlock() : nullptr)
    {
        if (block_)
            ++block_->weak;
    }
    WeakRef(const WeakRef& o)
        : p_(o.p_)
        , block_(o.block_)
    {
        if (block_)
            ++block_->weak;
    }
    WeakRef& operator=(WeakRef o) noexcept
    {
        std::swap(p_, o.p_);
        std::swap(block_, o.block_);
        return *this;
    }
    ~WeakRef()
    {
        if (block_ && --block_->weak == 0)
            delete block_;
    }

    RefPtr<T> Lock() const { return block_ && block_->alive ? RefPtr<T>(p_) : RefPtr<T>(); }

private:
    T* p_ = nullptr;
    RefBase::Block* block_ = nullptr;
};

// A native class with the custom handle.
struct Thing : RefBase
{
    int value = 0;
    explicit Thing(int v)
        : value(v)
    {
    }
};
