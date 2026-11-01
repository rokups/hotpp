// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

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
// "array_tpl" is the template of the type "T[N]" (the module attribute "array", std_ns.h). It is the
// internal form: the standard namespace is the placeholder.
ast::Script parse_script(std::string_view text, const LineMap& map, Diagnostics& diag,
    std::string_view array_tpl = kDefaultArrayTpl);

// Like parse_script. The module attribute list of the file can set another array template than
// "host_array_tpl". The first parse cannot know it, so the function parses the text again with the
// template of the file. The errors of the second parse replace the errors of the first.
ast::Script parse_script_array(std::string_view text, const LineMap& map, Diagnostics& diag,
    std::string_view host_array_tpl);

} // namespace hotc
