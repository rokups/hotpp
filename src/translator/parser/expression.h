// The parser of the expressions. This header has no parser-library include.
#pragma once

#include <vector>

#include "../ast.h"
#include "../diag.h"
#include "span.h"

namespace spc {

// Parses the text of the span as one expression. The function reads only the span.
// The span must hold the whole expression. White space and comments around it are fine.
// Text after the expression gives the error "expected end of expression".
//
// The result is null when the expression has a syntax error. The function adds each
// error to the diagnostics and never returns a partial tree for a syntax error.
// An error that a sub-parser records (for example in the body of a lambda) keeps the
// tree. So the caller must look at the diagnostics before it uses the tree.
// The positions in the nodes and in the errors are absolute byte offsets in the text.
ast::ExprP parse_expression(ParseInput in, Span span, Diagnostics& diag);

// Parses the text of the span as a list of expressions. A comma at depth 0 separates the
// items. A span with only white space gives an empty list. The rules of the errors are
// the same as for parse_expression. After a syntax error the list is empty. An empty
// list is also the result for an empty span, so look at the diagnostics.
std::vector<ast::ExprP> parse_expression_list(ParseInput in, Span span, Diagnostics& diag);

}  // namespace spc
