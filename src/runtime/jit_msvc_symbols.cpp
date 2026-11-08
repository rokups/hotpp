// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "jit_msvc_symbols.h"

#ifdef _MSC_VER

#include <climits>
#include <cstring>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <new>
#include <typeinfo>
#include <unordered_map>
#include <xthreads.h>

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

// The address of the thread variable of a control record. Other thread_local variables of the
// JIT code get a block for each thread. The block lives until the thread ends.
void* __cdecl emutls_get_address(EmutlsControl* c)
{
    if (c == &g_epoch_control)
        return &_Init_thread_epoch;
    thread_local std::unordered_map<EmutlsControl*, std::unique_ptr<void, AlignedFree>> blocks;
    auto it = blocks.find(c);
    if (it == blocks.end())
    {
        void* p = _aligned_malloc(c->size, c->align);
        if (!p)
            std::abort();
        if (c->value)
            std::memcpy(p, c->value, c->size);
        else
            std::memset(p, 0, c->size);
        it = blocks.emplace(c, std::unique_ptr<void, AlignedFree>(p)).first;
    }
    return it->second.get();
}

// "??3@YAXPEAX_K@Z": the sized operator delete. The size is not needed.
void __cdecl sized_delete(void* p, size_t)
{
    ::operator delete(p);
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
    add("??3@YAXPEAX_K@Z", reinterpret_cast<const void*>(&sized_delete));
    add("_Init_thread_header", reinterpret_cast<const void*>(&_Init_thread_header));
    add("_Init_thread_footer", reinterpret_cast<const void*>(&_Init_thread_footer));
    add("__emutls_v._Init_thread_epoch", &g_epoch_control);
    add("__emutls_get_address", reinterpret_cast<const void*>(&emutls_get_address));
    add("_Cnd_timedwait_for_unchecked", reinterpret_cast<const void*>(&_Cnd_timedwait_for_unchecked));
}

} // namespace hot::jit

#else

namespace hot::jit
{

void add_msvc_host_symbols(std::vector<const char*>&, std::vector<uint64_t>&) {}

} // namespace hot::jit

#endif
