// The parser of the script structure. This header has no parser-library include.
#pragma once

#include <string_view>

#include "../ast.h"
#include "../diag.h"

namespace spc {

// Parses the imports, classes, fields and methods of a script.
// The bodies of the methods and the initializers of the fields stay raw spans.
// The function adds each error to the diagnostics. It never throws for a bad script.
// The map must belong to the same text. The offsets in the errors are bytes in the text.
ast::Script parse_script(std::string_view text, const LineMap& map, Diagnostics& diag);

}  // namespace spc
