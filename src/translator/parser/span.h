// A byte range in the script text. This header has no parser-library include.
#pragma once

#include <cstdint>
#include <string_view>

namespace sppc {

struct LineMap;  // diag.h

// The begin and end are byte offsets in the script text. The end is not in the range.
struct Span {
    uint32_t begin = 0, end = 0;
};

// The input of a sub-parser. The text is the whole script buffer. The map belongs to
// the same text. A Span is a byte range in this text. A position in an error is an
// absolute byte offset in this text.
struct ParseInput {
    std::string_view text;
    const LineMap* map = nullptr;
};

}  // namespace sppc
