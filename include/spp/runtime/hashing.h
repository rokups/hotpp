// Hash of the shell layout.
#pragma once
#include "spp/runtime/hidden.h"

#include <cstdint>

namespace spp
{

// Hash of the shell layout. Two generations can share live objects only when
// the hash is equal. The hash covers the base class, the shell size and the
// number of native virtual functions (one stub in the shell for each).
SPP_HIDDEN constexpr uint64_t fnv1a(const char* s, uint64_t h = 14695981039346656037ull)
{
    for (; *s; ++s)
        h = (h ^ static_cast<uint8_t>(*s)) * 1099511628211ull;
    return h;
}

SPP_HIDDEN constexpr uint64_t hash_mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; ++i, v >>= 8)
        h = (h ^ (v & 0xff)) * 1099511628211ull;
    return h;
}

SPP_HIDDEN constexpr uint64_t shell_layout_hash(
    const char* base, uint64_t shell_size, uint64_t shell_align, uint64_t native_virtuals)
{
    return hash_mix(hash_mix(hash_mix(fnv1a(base), shell_size), shell_align), native_virtuals);
}

} // namespace spp
