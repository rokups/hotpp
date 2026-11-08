// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// MSVC only: the C runtime symbols that the JIT code needs and the process search cannot find.
#pragma once

#include <cstdint>
#include <vector>

namespace hot::jit
{

// Adds the names and the addresses of the symbols to the two lists. The host (/MD) supplies the
// addresses, so the JIT code uses the runtime and the heap of the host. The function does nothing
// on other compilers.
void add_msvc_host_symbols(std::vector<const char*>& names, std::vector<uint64_t>& values);

// Adds the host addresses of the allocator family (jit_host_allocators.h) to the two lists: the
// names of kHostAllocatorNames that this platform has. The calling code must be host code, so the
// addresses are the functions of the host C runtime. add_msvc_host_symbols calls it.
void add_host_allocator_symbols(std::vector<const char*>& names, std::vector<uint64_t>& values);

// One address in each C runtime DLL of the host: the C runtime (ucrtbase), the compiler runtime
// (vcruntime) and the C++ library (msvcp). The engine searches these DLLs first
// (hot_jit_engine_create). The calling code must be host code. The list is empty on other compilers.
std::vector<const void*> host_runtime_addresses();

} // namespace hot::jit
