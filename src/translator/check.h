// The checks of the script AST that need no libclang (design 5.3, 7.10.4 and 7.10.6).
// The pass runs on the parsed AST, before the lowering. It does not change the AST.
//
// It reports:
//  - a field, a property, a return type or a collection element that holds a borrowed
//    pointer or reference: "T*", "T&", "ref T" or "out T" of a type that is not a handle,
//  - a lambda that captures a "ref" or "out" parameter or a local that holds a borrowed
//    pointer or reference,
//  - a "ref e" that the script stores: the right side of an assignment, a "return" or a
//    field initializer,
//  - a script function used as a value (it has no address, because a reload replaces it).
//
// The pass sees only the types that the script writes. A "var" local gets its type from a
// native call, so the pass does not know it. See HANDOFF.md for the gaps.
#pragma once

#include <set>
#include <string>

#include "ast.h"
#include "diag.h"
#include "types.h"

namespace sppc {

// Checks one class. Each error goes to the diagnostics. The pass continues after an error.
// The findings of the borrowed-storage rule are errors with "strict_borrowed". Otherwise they
// are warnings. The rule on the address of a script function is always an error.
void check_class(const ast::ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag,
                 bool strict_borrowed);

// Checks the constructors of a class: one constructor, no "ref" or "out" parameter, and the
// arguments of "super(...)" use the constructor parameters only (design 7.3). Each error goes
// to the diagnostics.
void check_ctors(const ast::ClassDef& cd, const LineMap& map, Diagnostics& diag);

// The type names that a "new" expression names, in all classes of the script. The translator
// emits a "new" function for each script class in the set.
std::set<std::string> new_targets(const ast::Script& sc);

// The C++ type names that the script writes in declarations, "new" and lambda parameters.
// The translator asks libclang about them when the stub "class *;" is active.
std::set<std::string> used_type_names(const ast::Script& sc);
// The same for one class. The translator uses it to find the script classes that a class names.
std::set<std::string> used_type_names(const ast::ClassDef& cd);

}  // namespace sppc
