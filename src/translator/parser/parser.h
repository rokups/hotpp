// The parser of the script structure. This header has no parser-library include.
#pragma once

#include "../ast.h"
#include "../diag.h"

#include <string_view>

namespace hotc
{

// Parses the imports, classes, fields and methods of a script.
// The bodies of the methods and the initializers of the fields stay raw spans.
// The function adds each error to the diagnostics. It never throws for a bad script.
// The map must belong to the same text. The offsets in the errors are bytes in the text.
ast::Script parse_script(std::string_view text, const LineMap& map, Diagnostics& diag);

} // namespace hotc
