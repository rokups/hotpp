// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A byte range in the script text. This header has no parser-library include.
#pragma once

#include "../std_ns.h"

#include <cstdint>
#include <string_view>

namespace hotc
{

struct LineMap; // diag.h

// The begin and end are byte offsets in the script text. The end is not in the range.
struct Span
{
    uint32_t begin = 0, end = 0;
};

// The input of a sub-parser. The text is the whole script buffer. The map belongs to
// the same text. A Span is a byte range in this text. A position in an error is an
// absolute byte offset in this text.
struct ParseInput
{
    std::string_view text;
    const LineMap* map = nullptr;
    // The template of the type "T[N]" (the module attribute "array", std_ns.h). It is the internal form.
    std::string_view array_tpl = kDefaultArrayTpl;
};

} // namespace hotc
