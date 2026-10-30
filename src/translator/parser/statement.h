// The parser of the statements. This header has no parser-library include.
#pragma once

#include "../ast.h"
#include "../diag.h"
#include "span.h"

namespace sppc
{

// Parses the text of the span as the statements of a block. The span is the text between
// the braces. The pos of the block is the "{" before the span. The end_pos is the "}"
// after the span. The function adds each error to the diagnostics.
// The function in this step is a stub. It records an error and returns an empty block.
ast::BlockStmt parse_block(ParseInput in, Span body_without_braces, Diagnostics& diag);

} // namespace sppc
