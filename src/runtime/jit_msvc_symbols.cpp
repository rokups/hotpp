// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "jit_msvc_symbols.h"

#include "jit_host_allocators.h"

#include <cstddef>
#include <cstdlib>
#include <new>
#ifdef _MSC_VER
    #include <malloc.h>
#endif

namespace hot::jit
{

void add_host_allocator_symbols(std::vector<const char*>& names, std::vector<uint64_t>& values)
{
    // The address of the function of the host: this file is in hot_jit_backend (host code).
#define HOT_JIT_ADD(n, e)                                                                                           \
    names.push_back(n);                                                                                             \
    values.push_back(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<const void*>(e))));
    HOT_JIT_HOST_OPERATORS(HOT_JIT_ADD)
    HOT_JIT_HOST_C_HEAP(HOT_JIT_ADD)
#ifdef _MSC_VER
    HOT_JIT_HOST_MSVC_HEAP(HOT_JIT_ADD)
#endif
#undef HOT_JIT_ADD
}

} // namespace hot::jit

#ifdef _MSC_VER

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include <iostream>
#include <malloc.h>
#include <memory>
#include <new>
#include <typeinfo>
#include <unordered_map>
#include <xthreads.h>

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The engine DLL (hot_jit_engine) is /MT and the host is /MD. The process search of the engine
// reads only the exports of the loaded DLLs. These symbols are in the static part of the C
// runtime (msvcrt.lib, msvcprt.lib) and have no export. The host links that part, so the host
// gives the addresses. The JIT code then uses the same runtime state and the same heap as the host.
extern "C"
{
// The compiler calls these functions for a local static variable.
void __cdecl _Init_thread_header(int* once);
void __cdecl _Init_thread_footer(int* once);
// The epoch of the local statics of the thread. The JIT code reads it through emulated TLS.
extern __declspec(thread) int _Init_thread_epoch;
}

namespace hot::jit
{
namespace
{

// The control record of the emulated TLS (compiler-rt and libgcc use the same layout).
struct EmutlsControl
{
    size_t size;
    size_t align;
    uintptr_t index;
    const void* value;
};

// The JIT target uses emulated TLS: the COFF TLS of the JIT code has no support. The epoch must be
// the real variable of the host, because _Init_thread_footer writes it. A static record stands
// for it: the JIT code asks for the address of "__emutls_v._Init_thread_epoch".
EmutlsControl g_epoch_control = {sizeof(int), alignof(int), 0, nullptr};

struct AlignedFree
{
    void operator()(void* p) const { _aligned_free(p); }
};

// The key of a control record. The record of a removed module is free memory, and a record of a
// new module can get the same address. So the key is not the address. The first call gives the
// record a unique number in its "index" field, as compiler-rt does. The number of a new record is
// 0, because the JIT writes the record from the object.
uintptr_t emutls_key(EmutlsControl* c)
{
    static_assert(sizeof(c->index) == sizeof(long long), "the index is one 64-bit word");
    auto* slot = reinterpret_cast<volatile long long*>(&c->index);
    const long long known = _InterlockedCompareExchange64(slot, 0, 0);
    if (known != 0)
        return static_cast<uintptr_t>(known);
    static std::atomic<long long> next{0};
    const long long fresh = ++next;
    const long long old = _InterlockedCompareExchange64(slot, fresh, 0);
    return static_cast<uintptr_t>(old != 0 ? old : fresh);
}

// The address of the thread variable of a control record. Other thread_local variables of the
// JIT code get a block for each thread. The block lives until the thread ends. The block of a
// removed module stays until then too: its key does not come again.
void* __cdecl emutls_get_address(EmutlsControl* c)
{
    if (c == &g_epoch_control)
        return &_Init_thread_epoch;
    thread_local std::unordered_map<uintptr_t, std::unique_ptr<void, AlignedFree>> blocks;
    const uintptr_t key = emutls_key(c);
    auto it = blocks.find(key);
    if (it == blocks.end())
    {
        void* p = _aligned_malloc(c->size, c->align);
        if (!p)
            std::abort();
        if (c->value)
            std::memcpy(p, c->value, c->size);
        else
            std::memset(p, 0, c->size);
        it = blocks.emplace(key, std::unique_ptr<void, AlignedFree>(p)).first;
    }
    return it->second.get();
}

uint64_t address(const void* p)
{
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
}

} // namespace

void add_msvc_host_symbols(std::vector<const char*>& names, std::vector<uint64_t>& values)
{
    auto add = [&](const char* name, const void* value) {
        names.push_back(name);
        values.push_back(address(value));
    };
    // The first word of an object of std::type_info is the pointer to its virtual table.
    add("??_7type_info@@6B@", *reinterpret_cast<void* const*>(&typeid(int)));
    add("?__type_info_root_node@@3U__type_info_node@@A", &__type_info_root_node);
    add("_Init_thread_header", reinterpret_cast<const void*>(&_Init_thread_header));
    add("_Init_thread_footer", reinterpret_cast<const void*>(&_Init_thread_footer));
    add("__emutls_v._Init_thread_epoch", &g_epoch_control);
    add("__emutls_get_address", reinterpret_cast<const void*>(&emutls_get_address));
    add("_Cnd_timedwait_for_unchecked", reinterpret_cast<const void*>(&_Cnd_timedwait_for_unchecked));
    // The allocator family: the process search finds some of these names in other modules (an
    // export of another C runtime). Those use another heap than the host.
    add_host_allocator_symbols(names, values);
}

} // namespace hot::jit

namespace hot::jit
{

std::vector<const void*> host_runtime_addresses()
{
#ifndef _DLL
    // A /MT host has the runtime inside its own image. No runtime DLL belongs to the host.
    return {};
#else
    // The compiler defines choose the names of the runtime DLLs: _DLL (dynamic runtime) and _DEBUG
    // (suffix "d"). They are the flags of the host. All toolsets of VS2015 to VS2022 use "140".
    #define HOT_JIT_CRT_VER L"140"
    #ifdef _DEBUG
        #define HOT_JIT_CRT_D L"d"
    #else
        #define HOT_JIT_CRT_D L""
    #endif
    // A /MD build imports "fflush" (ucrtbase) and "std::cout" (msvcp) with dllimport, so each
    // address is in the DLL. The headers do not declare the vcruntime functions with dllimport: their
    // address is a jump stub in the host. So the base address of the loaded DLL stands for vcruntime.
    const HMODULE vcruntime = GetModuleHandleW(L"vcruntime" HOT_JIT_CRT_VER HOT_JIT_CRT_D L".dll");
    std::vector<const void*> out = {reinterpret_cast<const void*>(&std::fflush), static_cast<const void*>(vcruntime),
        static_cast<const void*>(&std::cout)};
    // The other parts of the runtime of the host, when they are loaded. The search of the JIT code
    // takes no name from a C runtime DLL that is not in this list (jit_search_order.h).
    const wchar_t* const parts[] = {L"vcruntime" HOT_JIT_CRT_VER L"_1" HOT_JIT_CRT_D L".dll",
        L"msvcp" HOT_JIT_CRT_VER L"_1" HOT_JIT_CRT_D L".dll", L"msvcp" HOT_JIT_CRT_VER L"_2" HOT_JIT_CRT_D L".dll",
        L"msvcp" HOT_JIT_CRT_VER HOT_JIT_CRT_D L"_atomic_wait.dll",
        L"msvcp" HOT_JIT_CRT_VER HOT_JIT_CRT_D L"_codecvt_ids.dll", L"concrt" HOT_JIT_CRT_VER HOT_JIT_CRT_D L".dll"};
    #undef HOT_JIT_CRT_D
    #undef HOT_JIT_CRT_VER
    for (const wchar_t* part : parts)
        if (const HMODULE module = GetModuleHandleW(part))
            out.push_back(static_cast<const void*>(module));
    return out;
#endif
}

} // namespace hot::jit

#else

namespace hot::jit
{

void add_msvc_host_symbols(std::vector<const char*>&, std::vector<uint64_t>&) {}

std::vector<const void*> host_runtime_addresses()
{
    return {};
}

} // namespace hot::jit

#endif
