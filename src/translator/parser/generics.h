// Script generics (doc/generics.md). The parser instantiates each generic declaration once for
// each set of type arguments that the module uses (monomorphization). An instantiation is a plain
// struct, class or free function with a generated name ("Box_G_int"). The later passes see only
// plain declarations.
#pragma once

#include "../diag.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace hotc
{

// A generic declaration at file level: "struct Box<T> { }", "class Stack<T> { }" or
// "T first<T>(T a, T b) { }". The scan removes the parameter list from the parse text.
struct GenericDecl
{
    enum class Kind
    {
        Struct,
        Class,
        Fn
    };
    Kind kind = Kind::Struct;
    std::string name;
    std::vector<std::string> params;
    uint32_t item_begin = 0, item_end = 0; // the byte range of the whole declaration
    uint32_t name_pos = 0;
};

// One instantiation: a generic and the canonical text of each type argument.
struct GenericInst
{
    size_t decl = 0;
    std::vector<std::string> args;
    std::string mangled; // the name of the plain declaration
    std::string display; // the name in messages: "Box<int>"
    uint32_t pos = 0; // the first use
    int depth = 0; // 1 for a use in plain code, n + 1 for a use in an instantiation of depth n
};

// The largest depth of an instantiation in an instantiation.
inline constexpr int kMaxGenericDepth = 16;

// The generics of one parse. The parser makes it current (generic_context) while it reads the
// types and the bodies. apply_aliases and split_name then replace each use by the generated name.
struct GenericContext
{
    std::vector<GenericDecl> decls;
    std::vector<GenericInst> insts;
    Diagnostics* diag = nullptr;
    const LineMap* map = nullptr;
    // The names of the file-level declarations. A generated name must not be one of them.
    std::set<std::string> names;
    // False while the parser reads a generic declaration for its syntax only: a use is then
    // checked but it adds no instantiation.
    bool record = true;
    int depth = 0; // the depth of the instantiation that the parser reads now, 0 for plain code
    uint32_t at = 0; // the position for an error of a use that has no own position

    const GenericDecl* find(const std::string& name) const;
    // A use "name<args>". "args" is the canonical text between the angle brackets. The result is
    // the generated name, or the text of the use when it has an error.
    std::string use(const std::string& name, const std::string& args, uint32_t pos);
    // A use of the generic "name" with no type arguments: an error.
    void bare(const std::string& name, uint32_t pos);
    void error(uint32_t pos, std::string msg) const;
};

// The current context of this thread, or null.
GenericContext*& generic_context();

// Finds the generic declarations of a ".hot" text. Each parameter list of a declaration becomes
// blank in "text" (the offsets stay). Reports the forms that version 1 does not support.
std::vector<GenericDecl> scan_generics(std::string& text, const LineMap& map, Diagnostics& diag);

// The positions of each use "name<" in a ".hot" text, outside comments and literals.
std::vector<uint32_t> find_generic_uses(const std::string& text, const std::string& name);

// Splits the text between the angle brackets at the commas of the top level. Trims each part.
std::vector<std::string> split_type_args(const std::string& inner);

// The display form of a canonical type text: "\x01std::string" gives "std.string".
std::string display_type(const std::string& t);

} // namespace hotc
