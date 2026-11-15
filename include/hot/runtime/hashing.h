// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Hash of the shell layout.
#pragma once
#include "hot/runtime/hidden.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hot
{

// Hash of the shell layout. Two generations can share live objects only when
// the hash is equal. The hash covers the base class, the shell size and the
// number of native virtual functions (one stub in the shell for each).
HOT_HIDDEN constexpr uint64_t fnv1a(const char* s, uint64_t h = 14695981039346656037ull)
{
    for (; *s; ++s)
        h = (h ^ static_cast<uint8_t>(*s)) * 1099511628211ull;
    return h;
}

HOT_HIDDEN constexpr uint64_t hash_mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; ++i, v >>= 8)
        h = (h ^ (v & 0xff)) * 1099511628211ull;
    return h;
}

// FNV-1a 64 of a text. The text can hold a zero byte. "seed" chains a hash over many texts.
constexpr uint64_t hash_text(std::string_view s, uint64_t h = 14695981039346656037ull)
{
    for (const char c : s)
        h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    return h;
}

// The hash of a script text for the AOT match (AotScript::source_hash). A line end "\r\n" or a
// lone "\r" counts as "\n", so a CRLF copy and an LF copy of a script give the same hash. For a
// text without "\r" the value is the same as hash_text. The line map and the diagnostics use the
// raw text, not this hash. Each maker and each check of a script hash must call this function.
constexpr uint64_t hash_script_text(std::string_view s)
{
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < s.size(); ++i)
    {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c == '\r')
        {
            c = '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
        }
        h = (h ^ c) * 1099511628211ull;
    }
    return h;
}

// The hash of the build environment of AOT code: the texts of the preludes in order, the format
// library, the view type and the standard option (empty: the standard of the host). The
// registration file of "hotc --aot" holds it (AotTable::env_hash). Never 0.
inline uint64_t aot_env_hash(const std::vector<std::string>& prelude_texts, std::string_view format_lib,
    std::string_view view_type, std::string_view std_version)
{
    uint64_t h = hash_mix(14695981039346656037ull, prelude_texts.size());
    for (const std::string& t : prelude_texts)
        h = hash_mix(hash_text(t, h), t.size());
    for (const std::string_view v : {format_lib, view_type, std_version})
        h = hash_mix(hash_text(v, h), v.size());
    return h ? h : 1;
}

HOT_HIDDEN constexpr uint64_t shell_layout_hash(
    const char* base, uint64_t shell_size, uint64_t shell_align, uint64_t native_virtuals)
{
    return hash_mix(hash_mix(hash_mix(fnv1a(base), shell_size), shell_align), native_virtuals);
}

} // namespace hot
