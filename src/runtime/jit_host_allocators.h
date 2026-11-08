// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The allocator family that JIT code takes from the host on Windows: all forms of the C++
// operators new and delete (MSVC x64 names) and the heap functions of the C runtime. So the JIT
// code and the host use one heap: a block that one side allocates, the other side can free.
// Each list item is X(name, address). The address expression is evaluated only in host code
// (jit_msvc_symbols.cpp). The engine (jit_engine.cpp) uses only the names: the expression is not
// expanded there.
#pragma once

#include <cstddef>

// The C++ operators. The expressions need <new> and <cstddef>.
#define HOT_JIT_HOST_OPERATORS(X)                                                                                   \
    X("??2@YAPEAX_K@Z", (static_cast<void* (*)(std::size_t)>(&::operator new)))                                    \
    X("??_U@YAPEAX_K@Z", (static_cast<void* (*)(std::size_t)>(&::operator new[])))                                 \
    X("??2@YAPEAX_KAEBUnothrow_t@std@@@Z",                                                                          \
        (static_cast<void* (*)(std::size_t, const std::nothrow_t&) noexcept>(&::operator new)))                      \
    X("??_U@YAPEAX_KAEBUnothrow_t@std@@@Z",                                                                         \
        (static_cast<void* (*)(std::size_t, const std::nothrow_t&) noexcept>(&::operator new[])))                    \
    X("??2@YAPEAX_KW4align_val_t@std@@@Z", (static_cast<void* (*)(std::size_t, std::align_val_t)>(&::operator new))) \
    X("??_U@YAPEAX_KW4align_val_t@std@@@Z",                                                                         \
        (static_cast<void* (*)(std::size_t, std::align_val_t)>(&::operator new[])))                                  \
    X("??2@YAPEAX_KW4align_val_t@std@@AEBUnothrow_t@1@@Z",                                                          \
        (static_cast<void* (*)(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept>(&::operator new)))    \
    X("??_U@YAPEAX_KW4align_val_t@std@@AEBUnothrow_t@1@@Z",                                                         \
        (static_cast<void* (*)(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept>(&::operator new[])))  \
    X("??3@YAXPEAX@Z", (static_cast<void (*)(void*) noexcept>(&::operator delete)))                                 \
    X("??_V@YAXPEAX@Z", (static_cast<void (*)(void*) noexcept>(&::operator delete[])))                              \
    X("??3@YAXPEAX_K@Z", (static_cast<void (*)(void*, std::size_t) noexcept>(&::operator delete)))                  \
    X("??_V@YAXPEAX_K@Z", (static_cast<void (*)(void*, std::size_t) noexcept>(&::operator delete[])))               \
    X("??3@YAXPEAXAEBUnothrow_t@std@@@Z",                                                                           \
        (static_cast<void (*)(void*, const std::nothrow_t&) noexcept>(&::operator delete)))                          \
    X("??_V@YAXPEAXAEBUnothrow_t@std@@@Z",                                                                          \
        (static_cast<void (*)(void*, const std::nothrow_t&) noexcept>(&::operator delete[])))                        \
    X("??3@YAXPEAXW4align_val_t@std@@@Z", (static_cast<void (*)(void*, std::align_val_t) noexcept>(&::operator delete))) \
    X("??_V@YAXPEAXW4align_val_t@std@@@Z",                                                                          \
        (static_cast<void (*)(void*, std::align_val_t) noexcept>(&::operator delete[])))                             \
    X("??3@YAXPEAX_KW4align_val_t@std@@@Z",                                                                         \
        (static_cast<void (*)(void*, std::size_t, std::align_val_t) noexcept>(&::operator delete)))                  \
    X("??_V@YAXPEAX_KW4align_val_t@std@@@Z",                                                                        \
        (static_cast<void (*)(void*, std::size_t, std::align_val_t) noexcept>(&::operator delete[])))                \
    X("??3@YAXPEAXW4align_val_t@std@@AEBUnothrow_t@1@@Z",                                                           \
        (static_cast<void (*)(void*, std::align_val_t, const std::nothrow_t&) noexcept>(&::operator delete)))        \
    X("??_V@YAXPEAXW4align_val_t@std@@AEBUnothrow_t@1@@Z",                                                          \
        (static_cast<void (*)(void*, std::align_val_t, const std::nothrow_t&) noexcept>(&::operator delete[])))

// The heap functions of the C standard. The expressions need <cstdlib>.
#define HOT_JIT_HOST_C_HEAP(X)                                                                                      \
    X("malloc", (static_cast<void* (*)(std::size_t)>(&std::malloc)))                                               \
    X("calloc", (static_cast<void* (*)(std::size_t, std::size_t)>(&std::calloc)))                                  \
    X("realloc", (static_cast<void* (*)(void*, std::size_t)>(&std::realloc)))                                      \
    X("free", (static_cast<void (*)(void*)>(&std::free)))

// The heap functions of the MSVC C runtime. The expressions need <malloc.h>.
#define HOT_JIT_HOST_MSVC_HEAP(X)                                                                                   \
    X("_msize", (&_msize))                                                                                          \
    X("_expand", (&_expand))                                                                                        \
    X("_aligned_malloc", (&_aligned_malloc))                                                                        \
    X("_aligned_free", (&_aligned_free))                                                                            \
    X("_aligned_realloc", (&_aligned_realloc))                                                                      \
    X("_aligned_msize", (&_aligned_msize))

namespace hot::jit
{
// One name of the allocator family. "msvc_only" names exist only in the MSVC C runtime.
struct HostAllocatorName
{
    const char* name;
    bool msvc_only;
};

#define HOT_JIT_PORTABLE_NAME(n, e) {n, false},
#define HOT_JIT_MSVC_NAME(n, e) {n, true},
inline constexpr HostAllocatorName kHostAllocatorNames[] = {
    HOT_JIT_HOST_OPERATORS(HOT_JIT_PORTABLE_NAME) HOT_JIT_HOST_C_HEAP(HOT_JIT_PORTABLE_NAME)
        HOT_JIT_HOST_MSVC_HEAP(HOT_JIT_MSVC_NAME)};
#undef HOT_JIT_PORTABLE_NAME
#undef HOT_JIT_MSVC_NAME

// True when "name" is in the allocator family.
inline bool is_host_allocator_name(const char* name, std::size_t length) noexcept
{
    for (const HostAllocatorName& a : kHostAllocatorNames)
    {
        std::size_t i = 0;
        while (i < length && a.name[i] && a.name[i] == name[i])
            ++i;
        if (i == length && !a.name[i])
            return true;
    }
    return false;
}
} // namespace hot::jit
