// The plain AST types of the script. The parser, the lowering pass and the printer share them.
// This header has no parser-library include and no libclang include.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "parser/span.h"

namespace sppc::ast {

using sppc::Span;

// How a parameter is passed. "ref" and "out" lower to a C++ reference.
enum class PassMode { Value, Ref, Out };

struct Param {
    std::string name, type;
    PassMode mode = PassMode::Value;
    // The type as the script writes it. The translator sets it before it rewrites "type" to the
    // C++ storage type. The lowering pass reads it for the type-directed member access.
    std::string script_type{};
    // The type of the parameter in the generation function of a script function, when the
    // parameter analysis (design 7.10.5) picks another form than "type". Empty: same as "type".
    std::string body_type{};
};

// ------------------------------------------------------------------ expressions
// A node is a plain struct with a pos field. A pos of 0 means: no source position.
// The pos of a node is the byte offset of its first token, unless the comment says
// otherwise. The emitter uses pos to find the script line of a leaf.
struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct NameExpr {  // a.b.c<int> in the script. The lowering pass decides: a scope or a member chain.
    std::vector<std::string> parts;
    std::string template_args;  // raw text with the angle brackets; may be empty
    // The arguments as expressions, when they parse as a list of expressions. The text in
    // template_args stays. The lowering pass lowers the nodes and rewrites the text.
    std::vector<ExprP> targ_exprs;
    bool targ_parsed = false;
    uint32_t pos = 0;
};
struct ThisExpr {
    uint32_t pos = 0;
};
struct SuperExpr {
    uint32_t pos = 0;
};
struct NullExpr {
    uint32_t pos = 0;
};
struct NumberExpr {
    std::string text;
    uint32_t pos = 0;
};
struct StringExpr {  // the text keeps the quotes
    std::string text;
    uint32_t pos = 0;
};
struct CharExpr {  // the text keeps the quotes
    std::string text;
    uint32_t pos = 0;
};
struct MemberExpr {
    ExprP obj;
    std::string name;  // empty: the script has "obj." and no name (a tolerant parse, with an error)
    uint32_t pos = 0;
    uint32_t name_pos = 0;  // the byte offset where the name starts (or would start)
};
struct CallExpr {
    ExprP callee;
    std::vector<ExprP> args;
    uint32_t pos = 0;
};
struct IndexExpr {
    ExprP obj, index;
    uint32_t pos = 0;
};
struct UnaryExpr {  // the pos is the pos of the operator, if it is a prefix operator
    std::string op;
    ExprP operand;
    bool postfix = false;
    uint32_t pos = 0;
};
struct RefExpr {  // "ref e" or "out e". The pos is the pos of the word.
    ExprP operand;
    bool is_out = false;
    uint32_t pos = 0;
};
struct BinaryExpr {  // also the assignment operators
    std::string op;
    ExprP lhs, rhs;
    uint32_t pos = 0;
};
struct TernaryExpr {
    ExprP cond, a, b;
    uint32_t pos = 0;
};
struct ParenExpr {
    ExprP inner;
    uint32_t pos = 0;
};
struct NewExpr {  // new T(a, b). The type is C++ text, for example "std::vector<float>".
    std::string type;
    std::vector<ExprP> args;
    uint32_t pos = 0;
};

// A block of statements. The pos is the "{". The end_pos is the "}".
struct BlockStmt {
    std::vector<StmtP> stmts;
    uint32_t pos = 0, end_pos = 0;
};

// "obj is T" or "obj as T" (design 7.7). The type is C++ text, as in NewExpr. The pos is
// the pos of the object. The lowering replaces the node with a call of a runtime helper.
struct TypeTestExpr {
    ExprP obj;
    std::string type;
    bool is_as = false;  // false: "is", true: "as"
    uint32_t pos = 0;
};

struct LambdaExpr {
    std::vector<Param> params;  // an empty type prints as "auto"
    BlockStmt body;
    uint32_t pos = 0;
};
// A string with "${expr}" or "${expr:spec}" (design 7.3, DECISIONS item 10). The literal parts
// keep the script text between the quotes (with escapes). "lits" has one more item than "args".
// The text is lits[0] args[0] lits[1] ... args[n-1] lits[n]. The lowering replaces the node with
// a call of std::format or fmt::format.
struct InterpExpr {
    std::vector<std::string> lits;
    std::vector<ExprP> args;
    std::vector<std::string> specs;  // the format spec of each arg, without the ":" (may be empty)
    uint32_t pos = 0;
};
struct RawExpr {  // C++ text that is already lowered
    std::string text;
    uint32_t pos = 0;
    // The script name in the text (design 10.1): text[name_at..] comes from the script name at
    // name_pos ("st->" + "hp" for "this.hp"). 0: the text has no separate name.
    uint32_t name_pos = 0;
    uint32_t name_at = 0;
};

struct Expr {
    std::variant<NameExpr, ThisExpr, SuperExpr, NullExpr, NumberExpr, StringExpr, CharExpr, MemberExpr,
                 CallExpr, IndexExpr, UnaryExpr, RefExpr, BinaryExpr, TernaryExpr, ParenExpr,
                 NewExpr, LambdaExpr, RawExpr, TypeTestExpr, InterpExpr>
        v;
};

// ------------------------------------------------------------------ statements
struct ExprStmt {
    ExprP expr;
    uint32_t pos = 0;
};
struct VarDeclStmt {
    bool is_const = false;
    std::string name;
    std::string type;  // empty: auto
    ExprP init;        // may be null
    uint32_t pos = 0;
    uint32_t name_pos = 0;  // the byte offset of the name (0: not known)
};
struct IfStmt {
    ExprP cond;
    StmtP then, otherwise;  // otherwise may be null
    uint32_t pos = 0, else_pos = 0;
};
struct WhileStmt {
    ExprP cond;
    StmtP body;
    uint32_t pos = 0;
};
struct DoWhileStmt {
    StmtP body;
    ExprP cond;
    uint32_t pos = 0, while_pos = 0;
};
struct ForStmt {
    std::vector<StmtP> init;  // VarDeclStmt or ExprStmt
    ExprP cond;               // may be null
    std::vector<ExprP> step;
    StmtP body;
    uint32_t pos = 0;
};
struct ForOfStmt {
    bool is_const = false;
    std::string name;
    std::string type;  // empty: auto
    ExprP range;
    StmtP body;
    uint32_t pos = 0;
    uint32_t name_pos = 0;  // the byte offset of the name (0: not known)
};
struct ReturnStmt {
    ExprP expr;  // may be null
    uint32_t pos = 0;
};
struct BreakStmt {
    uint32_t pos = 0;
};
struct ContinueStmt {
    uint32_t pos = 0;
};
struct SwitchCase {  // the pos is the first "case" or the "default"
    std::vector<ExprP> labels;
    // The pos of the "case" word of each label. It can be empty (a node built by hand).
    std::vector<uint32_t> label_pos;
    bool is_default = false;
    uint32_t default_pos = 0;  // the pos of the "default" word; 0: use pos
    std::vector<StmtP> stmts;
    uint32_t pos = 0;
};
struct SwitchStmt {
    ExprP subject;
    std::vector<SwitchCase> cases;
    uint32_t pos = 0, end_pos = 0;  // end_pos is the "}"
};
struct ThrowStmt {
    ExprP expr;  // null: rethrow
    uint32_t pos = 0;
};
// One catch clause. The pos is the "catch" word. A catch-all clause has no type and no name.
struct CatchClause {
    std::string type;  // the C++ type text, for example "const std::exception&"
    std::string name;
    bool is_all = false;
    BlockStmt block;
    uint32_t pos = 0;
};
// A try statement has one or more catch clauses, a finally block, or both.
struct TryStmt {
    BlockStmt block;
    std::vector<CatchClause> catches;  // zero or more; zero only with a finally block
    std::optional<BlockStmt> finally_block;
    uint32_t pos = 0;
};
struct EmptyStmt {
    uint32_t pos = 0;
};
struct RawStmt {  // C++ text that is already lowered
    std::string text;
    uint32_t pos = 0;
};

struct Stmt {
    std::variant<BlockStmt, ExprStmt, VarDeclStmt, IfStmt, WhileStmt, DoWhileStmt, ForStmt, ForOfStmt,
                 ReturnStmt, BreakStmt, ContinueStmt, SwitchStmt, ThrowStmt, TryStmt, EmptyStmt, RawStmt>
        v;
};

// Put a node in a new Expr or Stmt.
template <class T>
ExprP make_expr(T node)
{
    auto p = std::make_unique<Expr>();
    p->v = std::move(node);
    return p;
}
template <class T>
StmtP make_stmt(T node)
{
    auto p = std::make_unique<Stmt>();
    p->v = std::move(node);
    return p;
}

inline uint32_t pos_of(const Expr& e)
{
    return std::visit([](const auto& n) { return n.pos; }, e.v);
}
inline uint32_t pos_of(const Stmt& s)
{
    return std::visit([](const auto& n) { return n.pos; }, s.v);
}

// ------------------------------------------------------------------ script
// The access modifier of a member. The parser stores it. Nothing checks it yet.
enum class Access { Public, Private, Protected };

struct Field {
    std::string name, alias, type;  // alias: the previous name from [alias("...")]
    std::string script_type;        // "type" as the script writes it (see Param)
    ExprP init;      // null when the field has no initializer, or the parse failed
    Span init_span;  // the raw text after "=", up to the ";"
    bool has_init = false;
    Access access = Access::Public;
    int line = 0;
    uint32_t pos = 0;
};

struct Method {
    // True when the function overrides a base virtual method. The translator sets it after
    // the match. The parser leaves it false.
    bool overrides_virtual = false;
    // The modifiers as written. The parser stores them. Nothing checks them yet.
    bool is_override = false, is_virtual = false, is_new = false;
    // The script class that owns the virtual slot of the function (design 7.6): the class
    // with the "virtual" declaration. A call goes through the slot then. The translator sets
    // it for a "virtual" function and for an "override" of a script virtual function. Empty:
    // a direct call.
    std::string vslot_owner;
    Access access = Access::Public;
    std::string name;
    std::vector<Param> params;
    std::string ret;  // empty: not written
    std::string script_ret;  // "ret" as the script writes it (see Param)
    BlockStmt body;  // the statements; the parser fills them from body_span
    Span body_span;  // the raw text between the braces
    int line = 0, end_line = 0;
    uint32_t pos = 0, end_pos = 0;
};

// A constructor: "Name(T a) : super(args) { body }". The name is the name of the class.
// The super call is optional. Its arguments may use the parameters only (checked by the
// translator). The body runs after the state block has its field initializers.
struct Ctor {
    std::vector<Param> params;
    Access access = Access::Public;
    bool has_super = false;
    std::vector<ExprP> super_args;
    Span super_span;  // the raw text between the parentheses of "super(...)"
    BlockStmt body;   // the statements; the parser fills them from body_span
    Span body_span;   // the raw text between the braces
    int line = 0, end_line = 0;
    uint32_t pos = 0, end_pos = 0, super_pos = 0;  // the name, the "}", the "super" word
};

// A property: "Type Name { get; set; }" or "Type Name { get { ... } set { ... } }".
// An accessor without a body is an auto accessor. An auto property can have "= init;".
// The access on an accessor is parsed and stored. Nothing checks it yet.
struct Property {
    std::string name, type;
    std::string script_type;  // "type" as the script writes it (see Param)
    Access access = Access::Public;
    bool has_get = false, has_set = false;
    Access get_access = Access::Public, set_access = Access::Public;
    // A custom accessor has a body. The parser fills the body from the span.
    std::optional<BlockStmt> get_body, set_body;
    Span get_span, set_span;  // the raw text between the braces of a custom accessor
    ExprP init;               // null when the property has no initializer, or the parse failed
    Span init_span;           // the raw text after "=", up to the ";"
    bool has_init = false;
    int line = 0;
    uint32_t pos = 0, get_pos = 0, set_pos = 0;  // the name, the "get" word, the "set" word
    bool is_auto() const { return !get_body && !set_body; }
};

struct ClassDef {
    std::string name, base;
    // The native interface bases after the primary base: "class Boss : Actor, IDamageable"
    // (design 4.4). The primary base is "base".
    std::vector<std::string> interfaces;
    std::vector<Field> fields;
    std::vector<Method> methods;
    std::vector<Ctor> ctors;      // the overloads; C++ picks one by the argument types
    std::vector<Method> dtors;    // "~Name() { }": the name is "~Name"; the translator allows one
    std::vector<Property> props;  // in source order
    bool is_value = false;        // the attribute [value]
    int line = 0;
    uint32_t pos = 0;
};

// A class stub: a native class with no body. The attributes give the storage kind.
// The name may end with "<*>" or "::*", or it may be "*".
struct Stub {
    std::string name;
    bool is_native = false, is_value = false;
    std::string handle, default_handle;
    std::string intrusive_add, intrusive_release;
    int line = 0;
    uint32_t pos = 0;
};

// A script enum: "enum Name : type { A, B = 5 }". It becomes a C++ "enum class". The
// underlying type is a builtin integer type; empty means int32_t. A value is optional.
struct Enumerator {
    std::string name;
    ExprP value;      // null when the enumerator has no value, or the parse failed
    Span value_span;  // the raw text after "="
    bool has_value = false;
    int line = 0;
    uint32_t pos = 0;
};

struct EnumDef {
    std::string name, underlying;  // underlying: C++ type text, or empty
    std::vector<Enumerator> items;
    int line = 0;
    uint32_t pos = 0, underlying_pos = 0;
};

struct Import {
    std::string path;  // the header name; a system header keeps its angle brackets
    int line;
    uint32_t pos = 0;
    // The text after #include: <vector> for a system header, "Actor.h" for the others.
    std::string target() const
    {
        return !path.empty() && path.front() == '<' ? path : "\"" + path + "\"";
    }
};

struct Script {
    std::vector<Import> imports;
    std::vector<ClassDef> classes;
    std::vector<Stub> stubs;  // in source order
    std::vector<EnumDef> enums;
    // Module variables: "Type name = init;" at file level. The translator collects them in
    // the module state block.
    std::vector<Field> vars;
    // Convert hooks of module variables: "T convert_<var>(Old old) { }" at file level.
    std::vector<Method> var_hooks;
};

}  // namespace sppc::ast
