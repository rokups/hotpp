// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The table of the resolved names (hotc::ResolvedRef in hot/hotc_lib.h). A language server uses
// it for rename, references, definition and hover.
//
// The pass is read only. It walks the parsed AST of the main script after the merge of the
// imports and before the lowering. It has the same scope rules as the lowering (lower.h): a
// local hides a member of the class, a member of the class hides a module variable, and a type
// name is the last choice. It keeps a stack of scopes with the declaration offset of each local,
// so two locals with the same name get different ids. The generated C++ does not change.
#pragma once

#include "ast.h"
#include "diag.h"

#include "hot/hotc_lib.h"

#include <functional>
#include <string>
#include <vector>

namespace hotc
{

struct RefInputs
{
    const ast::Script* script = nullptr; // the main script, with the classes, enums and constants of the imports
    const std::vector<ast::Method>* foreign_fns = nullptr; // the free functions of the direct imports (can be null)
    // The free functions of the imports of the imports (can be null). The lowering does not see
    // them, but a rename must find each use. A function of "foreign_fns" wins over one of them.
    const std::vector<ast::Method>* deep_fns = nullptr;
    // The module variables of all the imports: the direct imports first.
    std::vector<const ast::Field*> foreign_vars;
    std::string_view text; // the text of the main script
    const LineMap* map = nullptr;
    // The normal path of the main script (hotc::ref_file_id).
    std::string main_file;
    // The normal path of the imported script that holds an offset. Empty for the main script.
    std::function<std::string(uint32_t)> file_of;
};

struct RefTable
{
    std::vector<ResolvedRef> refs; // sorted by offset; a name gets one ref at most
    std::vector<UnresolvedName> unresolved; // sorted by offset
    std::vector<std::pair<uint32_t, uint32_t>> raw_ranges; // the begin and the end of each raw body
};

RefTable resolve_refs(const RefInputs& in);

} // namespace hotc
