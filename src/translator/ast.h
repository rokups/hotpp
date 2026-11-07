// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The plain AST types of the script. The parser, the lowering pass and the printer share them.
// This header has no parser-library include and no libclang include.
#pragma once

#include "parser/span.h"

#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hotc::ast
{

// The error of a function "main" with a form that the host cannot call.
inline constexpr const char* kMainSignatureError =
    "the function 'main' must be 'void main()', 'int main()', 'void main(std.string[] args)' or "
    "'int main(std.string[] args)'";

// The nested namespace of a namespace "ns" of a ".cpp" script. It holds the names that hotc
// generates for the script classes of "ns". It is empty for the file scope.
inline std::string gen_namespace(const std::string& ns)
{
    return ns.empty() ? ns : ns + "::hot_gen";
}

using hotc::Span;

// True when a parameter type is a "T&" (a non-const lvalue reference). A "T&&" and a
// "const T&" are not. The translator rewrites a handle value type to "const H&": it stays false.
inline bool is_ref_type(const std::string& type)
{
    size_t e = type.find_last_not_of(" \t");
    if (e == std::string::npos || type[e] != '&')
        return false;
    if (e > 0 && type[e - 1] == '&')
        return false;
    return type.compare(0, 6, "const ") != 0;
}

// True when a return type text starts with the word "ref": "ref T". The parser can write "ref$T" for a
// handle type, so the text after the word can start with "$".
inline bool is_ref_return_text(const std::string& type)
{
    if (type.compare(0, 3, "ref") != 0 || type.size() == 3)
        return false;
    const unsigned char c = static_cast<unsigned char>(type[3]);
    return !(std::isalnum(c) || c == '_');
}

// The type text after the word "ref" of a "ref T" return type.
inline std::string strip_ref_return_word(const std::string& type)
{
    size_t i = 3;
    while (i < type.size() && type[i] == ' ')
        ++i;
    return type.substr(i);
}

// The default value of a parameter: "T name = e". The copies of a parameter share it. The parser
// fills "expr" and "span". The lowering fills "cpp" one time (empty until then).
struct ParamDefault;

// The modifier of a parameter. "in T x" is a const reference, "ref T x" and "out T x" are references,
// "[byval] T x" is a copy. "Plain" is a parameter with no modifier.
enum class ParamMode
{
    Plain,
    In,
    Ref,
    Out,
    ByVal
};

struct Param
{
    // The parser keeps the modifier in "mode" and writes the C++ form of it into "type": "const T&"
    // for In, "T&" for Ref and Out. The code after the parser reads the C++ form. A C++ import has
    // the mode Plain and a type with a "&".
    std::string name, type;
    ParamMode mode = ParamMode::Plain;
    std::shared_ptr<ParamDefault> def{}; // null: the parameter has no default value
    // The type as the script writes it. The translator sets it before it rewrites "type" to the
    // C++ storage type. The lowering pass reads it for the type-directed member access.
    std::string script_type{};
    // The type of the parameter in the generation function of a script function, when the
    // parameter analysis (design 7.10.5) picks another form than "type". Empty: same as "type".
    std::string body_type{};
    // True when the body never writes the parameter (const_infer.h). The body form has "const". The
    // signature and the slot do not change.
    bool body_const = false;
    // An override that matched a native virtual through the handle carriers (task 33): the native
    // parameter type text. The slot keeps this type. Empty: the type is not relaxed.
    std::string native_type{};
    // With native_type: the handle storage type of the script parameter. The first line of the body
    // makes the handle with hot::bind<bind_to>(native argument).
    std::string bind_to{};
    // "[weak] T name": the parameter holds a weak handle (task 38). The storage is a weak_ptr.
    bool weak = false;
    // The type is a bare script class: the parameter holds a box (hot::Value<T>). The resolution of
    // the value types sets it. The parser does not.
    bool value = false;
};

// ------------------------------------------------------------------ expressions
// A node is a plain struct with a pos field. A pos of 0 means: no source position.
// The pos of a node is the byte offset of its first token, unless the comment says
// otherwise. The emitter uses pos to find the script line of a leaf.
struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct NameExpr
{ // a.b.c<int> in the script. The lowering pass decides: a scope or a member chain.
    std::vector<std::string> parts;
    std::string template_args; // raw text with the angle brackets; may be empty
    // The arguments as expressions, when they parse as a list of expressions. The text in
    // template_args stays. The lowering pass lowers the nodes and rewrites the text.
    std::vector<ExprP> targ_exprs;
    bool targ_parsed = false;
    uint32_t pos = 0;
    // The script offset of the last part of the name. The value is absolute. 0 means unknown.
    // The raw text of the earlier parts can differ from parts[i] (see split_name).
    uint32_t last_pos = 0;
    // The script offset of each part, in the order of parts. Empty when unknown.
    std::vector<uint32_t> part_pos;
    // The lowering: the C++ text of a "foreach" variable that the guard gives at each use, for
    // example "__it0.ref()" (foreach.h). Empty: the normal name.
    std::string emit_as{};
    // The lowering: a "ref" local that is a checked path (VarDeclStmt::path). The emitter writes
    // this expression at each use. Null: the normal name.
    const Expr* emit_path = nullptr;
    std::string emit_path_view{}; // a view path (B7): the C++ view type around the path
};
struct ThisExpr
{
    uint32_t pos = 0;
};
struct BaseExpr
{
    uint32_t pos = 0;
};
struct NullExpr
{
    uint32_t pos = 0;
};
struct NumberExpr
{
    std::string text;
    uint32_t pos = 0;
};
struct StringExpr
{ // the text keeps the quotes
    std::string text;
    uint32_t pos = 0;
};
struct CharExpr
{ // the text keeps the quotes
    std::string text;
    uint32_t pos = 0;
};
struct MemberExpr
{
    ExprP obj;
    std::string name; // empty: the script has "obj." and no name (a tolerant parse, with an error)
    uint32_t pos = 0;
    uint32_t name_pos = 0; // the byte offset where the name starts (or would start)
    bool safe = false; // "obj?.name": the lowering writes hot::safe_access, or an "if" at the start of a statement
};
struct CallExpr
{
    ExprP callee;
    std::vector<ExprP> args;
    uint32_t pos = 0;
};
struct IndexExpr
{
    ExprP obj, index;
    uint32_t pos = 0;
};
struct UnaryExpr
{ // the pos is the pos of the operator, if it is a prefix operator
    std::string op;
    ExprP operand;
    bool postfix = false;
    uint32_t pos = 0;
};
struct BinaryExpr
{ // also the assignment operators
    std::string op;
    ExprP lhs, rhs;
    uint32_t pos = 0;
};
struct TernaryExpr
{
    ExprP cond, a, b;
    uint32_t pos = 0;
};
struct ParenExpr
{
    ExprP inner;
    uint32_t pos = 0;
};
struct NewExpr
{ // new T(a, b). The type is C++ text, for example "std::vector<float>".
    std::string type;
    std::vector<ExprP> args;
    uint32_t pos = 0;
};

// The shared part of a parameter with a default value.
struct ParamDefault
{
    ExprP expr; // the script expression (null after the lowering moved it)
    Span span; // the raw text after "="
    std::string cpp; // the lowered C++ text, when "lowered" is true
    bool lowered = false;
    uint32_t pos = 0; // the byte offset of the "="
};

// A block of statements. The pos is the "{". The end_pos is the "}".
struct BlockStmt
{
    std::vector<StmtP> stmts;
    uint32_t pos = 0, end_pos = 0;
};

// "obj is T" or "obj as T" (design 7.7). The type is C++ text, as in NewExpr. The pos is
// the pos of the object. The lowering replaces the node with a call of a runtime helper.
struct TypeTestExpr
{
    ExprP obj;
    std::string type;
    bool is_as = false; // false: "is", true: "as"
    uint32_t pos = 0;
    // "obj is T name": the name of the binding. Empty: no binding. The lowering of an if or a
    // while moves the binding to the condition. Any other place is an error.
    std::string bind{};
    uint32_t bind_pos = 0;
};

struct LambdaExpr
{
    std::vector<Param> params; // an empty type prints as "auto"
    BlockStmt body;
    uint32_t pos = 0;
    std::string captures; // the lowering: init-captures after "[=", for example ", a = a"
    // The names outside the lambda that the body assigns, increments or decrements (const_infer.h).
    // The lowering gives an error when such a name is not a shared cell.
    std::set<std::string> assigns{};
};
// A string with "${expr}" or "${expr:spec}" (design 7.3, DECISIONS item 10). The literal parts
// keep the script text between the quotes (with escapes). "lits" has one more item than "args".
// The text is lits[0] args[0] lits[1] ... args[n-1] lits[n]. The lowering replaces the node with
// a call of std::format or fmt::format.
struct InterpExpr
{
    std::vector<std::string> lits;
    std::vector<ExprP> args;
    std::vector<std::string> specs; // the format spec of each arg, without the ":" (may be empty)
    uint32_t pos = 0;
};
// A collection literal: "[1, 2, 3]" or "["a": 1, "b": 2]". A list has only "elems". A map
// has the keys in "elems" and the values in "values". The pos is the "[". The end_pos is the "]".
// The lowering accepts the literal only where the type is declared. The C++ text is a braced list.
struct CollectionExpr
{
    std::vector<ExprP> elems;
    std::vector<ExprP> values;
    bool is_map = false;
    uint32_t pos = 0, end_pos = 0;
};
struct RawExpr
{ // C++ text that is already lowered
    std::string text;
    uint32_t pos = 0;
    // The script name in the text (design 10.1): text[name_at..] comes from the script name at
    // name_pos ("st->" + "hp" for "this.hp"). 0: the text has no separate name.
    uint32_t name_pos = 0;
    uint32_t name_at = 0;
};

struct Expr
{
    std::variant<NameExpr, ThisExpr, BaseExpr, NullExpr, NumberExpr, StringExpr, CharExpr, MemberExpr, CallExpr,
        IndexExpr, UnaryExpr, BinaryExpr, TernaryExpr, ParenExpr, NewExpr, LambdaExpr, RawExpr, TypeTestExpr,
        InterpExpr, CollectionExpr>
        v;
};

// ------------------------------------------------------------------ statements
struct ExprStmt
{
    ExprP expr;
    uint32_t pos = 0;
};
struct VarDeclStmt
{
    // True when the const inference (const_infer.h) finds that the body never writes the local. The
    // script never writes "const" on a local. The parser leaves it false.
    bool is_const = false;
    bool is_ref = false; // "ref x = e": a local reference, the C++ text is "auto&"
    std::string name;
    std::string type; // empty: auto
    ExprP init; // may be null
    uint32_t pos = 0;
    uint32_t name_pos = 0; // the byte offset of the name (0: not known)
    // The C++ type of a "var" local that libclang gives for a native initializer (S20b), without
    // a reference and a top-level "const", as "auto" deduces it. Empty: not known. The emitter
    // does not print it. The checks and the type-directed lowering read it.
    std::string native_type{};
    // "var (a, b) = e": a structured binding. The names are here and "name" is empty. The C++
    // text is "auto [a, b]". "type" is empty. "ref" sets "is_ref".
    std::vector<std::string> names{};
    std::vector<uint32_t> name_poss{}; // the byte offset of each name in "names"
    // The lowering: "ref r = xs[i]" is a checked path (doc/type-design.md, "Lifetime of a ref").
    // "init" is the path. Each use of the name writes the path again. "path_caches" are the
    // "const auto __rN_k = i;" locals of the indices. The emitter writes the caches and
    // "(void)(path);" for the check at the declaration.
    bool path = false;
    std::vector<StmtP> path_caches{};
    // "[weak] T x = e": the local holds a weak handle (task 38). The storage is a weak_ptr.
    bool weak = false;
    uint32_t weak_pos = 0; // the byte offset of "weak" (0: not known)
    // The type is a bare script class: the local holds a box (hot::Value<T>). The resolution of
    // the value types sets it. The parser does not.
    bool value = false;
    // True when a lambda writes the local (const_infer.h, G06). The local lives in a shared cell:
    // "auto __cell_x = hot::make_cell<T>(init); T& x = *__cell_x;". The parser leaves it false.
    bool shared = false;
};
struct IfStmt
{
    ExprP cond;
    StmtP then, otherwise; // otherwise may be null
    uint32_t pos = 0, else_pos = 0;
};
struct WhileStmt
{
    ExprP cond;
    StmtP body;
    uint32_t pos = 0;
};
struct DoWhileStmt
{
    StmtP body;
    ExprP cond;
    uint32_t pos = 0, while_pos = 0;
};
struct ForStmt
{
    std::vector<StmtP> init; // VarDeclStmt or ExprStmt
    ExprP cond; // may be null
    std::vector<ExprP> step;
    StmtP body;
    uint32_t pos = 0;
};
struct ForOfStmt
{
    // True for a "ref" target that the body never writes (const_infer.h). The C++ text is "const auto&".
    bool is_const = false;
    bool is_ref = false; // "foreach (ref x in c)": the C++ text is "auto&"
    std::string name;
    std::string type; // empty: auto
    // The type is a bare script class: the variable holds a box. The resolution of the value
    // types sets it. The parser does not.
    bool value = false;
    ExprP range;
    StmtP body;
    uint32_t pos = 0;
    uint32_t name_pos = 0; // the byte offset of the name (0: not known)
    // "foreach (var (k, v) in c)": a structured binding. "name" is empty. See VarDeclStmt.
    std::vector<std::string> names{};
    // The lowering: the name of the run-time guard (foreach.h). Empty: a plain range-for.
    std::string guard{};
    bool guard_path = false; // the range is a checked path: "hot::iter_path" evaluates it again
    bool guard_enum = false; // "enumerate(c)": the range is "c", the binding is (index, element)
    // A call or a "new" at the root of the range ("this.Make().items"). The guard keeps the
    // value alive. The range uses the name "guard_root_name" in its place ("hot::iter_root").
    ExprP guard_root{};
    std::string guard_root_name{};
    std::vector<uint32_t> name_poss{};
};
struct ReturnStmt
{
    ExprP expr; // may be null
    uint32_t pos = 0;
    // True in the body of an "async" method: the statement prints as "co_return". The lowering sets it.
    bool co = false;
};
struct BreakStmt
{
    uint32_t pos = 0;
};
struct ContinueStmt
{
    uint32_t pos = 0;
};
struct SwitchCase
{ // the pos is the first "case" or the "default"
    std::vector<ExprP> labels;
    // The pos of the "case" word of each label. It can be empty (a node built by hand).
    std::vector<uint32_t> label_pos;
    bool is_default = false;
    uint32_t default_pos = 0; // the pos of the "default" word; 0: use pos
    std::vector<StmtP> stmts;
    uint32_t pos = 0;
};
struct SwitchStmt
{
    ExprP subject;
    std::vector<SwitchCase> cases;
    uint32_t pos = 0, end_pos = 0; // end_pos is the "}"
};
struct ThrowStmt
{
    ExprP expr; // null: rethrow
    uint32_t pos = 0;
};
// One catch clause. The pos is the "catch" word. A catch-all clause has no type and no name.
struct CatchClause
{
    std::string type; // the C++ type text, for example "const std::exception&"
    std::string name;
    bool is_all = false;
    BlockStmt block;
    uint32_t pos = 0;
};
// A try statement has one or more catch clauses, a finally block, or both.
struct TryStmt
{
    BlockStmt block;
    std::vector<CatchClause> catches; // zero or more; zero only with a finally block
    std::optional<BlockStmt> finally_block;
    uint32_t pos = 0;
    std::string finally_captures; // the lowering: init-captures after "[&", for example ", &a = a"
    // The lowering: a catch clause for ScriptError or NativeError also takes a native exception
    // (std::exception) from the block. The block gets an inner handler that converts it.
    bool convert_native = false;
};
struct EmptyStmt
{
    uint32_t pos = 0;
};
struct RawStmt
{ // C++ text that is already lowered
    std::string text;
    uint32_t pos = 0;
};

struct Stmt
{
    std::variant<BlockStmt, ExprStmt, VarDeclStmt, IfStmt, WhileStmt, DoWhileStmt, ForStmt, ForOfStmt, ReturnStmt,
        BreakStmt, ContinueStmt, SwitchStmt, ThrowStmt, TryStmt, EmptyStmt, RawStmt>
        v;
};

// Put a node in a new Expr or Stmt.
template <class T> ExprP make_expr(T node)
{
    auto p = std::make_unique<Expr>();
    p->v = std::move(node);
    return p;
}
template <class T> StmtP make_stmt(T node)
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
enum class Access
{
    Public,
    Private,
    Protected
};

// A script attribute tag on a field: "[Name]" or "[Name("a", "b")]".
struct FieldTag
{
    std::string name;
    std::vector<std::string> args;
    uint32_t pos = 0; // the byte offset of the name
};

struct Field
{
    std::string name, alias, type; // alias: the previous name from [alias("...")]
    std::vector<FieldTag> attributes; // the tags in source order
    std::string script_type; // "type" as the script writes it (see Param)
    bool weak = false; // "[weak]": the field holds a weak handle (task 38)
    bool value = false; // the type is a bare script class: the field holds a box (hot::Value<T>)
    ExprP init; // null when the field has no initializer, or the parse failed
    Span init_span; // the raw text after "=", up to the ";"
    bool has_init = false;
    Access access = Access::Public;
    int line = 0;
    uint32_t pos = 0;
    // For a ".cpp" script. When "raw" is true, the translator uses the raw text and not the AST.
    bool raw = false;
    std::string raw_init; // the edited initializer text
    bool raw_brace_init = false; // true when the initializer is "{...}"
    // A module variable of a ".cpp" script in a namespace: the namespace path ("game::ai"). The
    // module block is at file scope, so the translator declares the type in the namespace.
    std::string ns;
    // A class constant ("static const T Name = value;" in a class body): the name of the class.
    // The parser moves the constant to Script::consts. "name" is then "Class__Name". A static
    // field ("static T Name;", gap G23) has an owner too: the parser moves it to Script::vars.
    // Empty: a module constant, a module variable or a field.
    std::string owner;
};

// A line of raw C++ text with the script line where it starts.
struct RawLine
{
    int line = 0;
    std::string text;
};

// "static_assert(cond, "message");" at file level or in a class body (gap G08). The parser
// moves a class item to Script::static_asserts and sets the owner. The translator writes each
// one at file scope after the types, so a script struct is complete there.
struct StaticAssert
{
    Span args_span; // the text between the parentheses
    std::vector<ExprP> args; // the condition and the message, after the parse
    std::string owner; // the class of a class item, else empty
    int line = 0;
    uint32_t pos = 0;
};

struct Method
{
    // True when the function overrides a base virtual method. The translator sets it after
    // the match. The parser leaves it false.
    bool overrides_virtual = false;
    // The key of the native virtual method that the function overrides (empty when it overrides
    // none). The interface rows use it to find the exact slot of an overload.
    std::string native_key;
    // The modifiers as written. The parser stores them. Nothing checks them yet.
    bool is_override = false, is_virtual = false, is_new = false;
    // True for a const method of a ".cpp" script class.
    bool is_const = false;
    // True when the script writes "abstract" before the method. The method has no body and it
    // implies "virtual". The parser leaves "is_virtual" false. Only a class with "is_abstract" has it.
    bool is_abstract = false;
    // True when the script writes "async" before the method. The method can use "await".
    bool is_async = false;
    // True when the script writes "static" before the method. The method has no object. The parser
    // stores it in "ClassDef::statics", not in "ClassDef::methods".
    bool is_static = false;
    // True when the body of a ".cpp" method is a C++20 coroutine (co_await, co_return). The
    // ".cpp" frontend sets it. The member of the state then has no stale return and no trace frame.
    bool is_coroutine = false;
    // The script class that owns the virtual slot of the function (design 7.6): the class
    // with the "virtual" declaration. A call goes through the slot then. The translator sets
    // it for a "virtual" function and for an "override" of a script virtual function. Empty:
    // a direct call.
    std::string vslot_owner;
    Access access = Access::Public;
    std::string name;
    // The name as the script writes it, for a method of an overload group (gap G17). Empty
    // for other methods. The translator then sets "name" to a unique key "<name>__ov_<sig>":
    // the slot, the state member and the thunk use the key. The calls use this name, and
    // C++ forwarders of this name select the overload by the argument types.
    std::string src_name;
    const std::string& written_name() const { return src_name.empty() ? name : src_name; }
    // The operator symbol ("+", "[]", ...) when the method is an operator. Empty otherwise. The
    // "name" is then the identifier of the operator (see operator_names.h).
    std::string op;
    std::vector<Param> params;
    std::string ret; // empty: not written
    std::string script_ret; // "ret" as the script writes it (see Param)
    bool ret_value = false; // the return type is a bare script class: the method returns a box
    // True when the script writes "ref T" as the return type. The lowering maps "ret" to "T&". The
    // checker applies the return rule of doc/type-design.md ("Lifetime of a ref", R4).
    bool ret_ref = false;
    BlockStmt body; // the statements; the parser fills them from body_span
    // The unit of the module that owns a free function of an import. The function has no body
    // here: the call goes through the function table of that unit (hot::call_fn). Empty: own function.
    std::string foreign_unit;
    Span body_span; // the raw text between the braces
    // True for "T f() => e;". The body_span is then the text of "e" and the parser builds the
    // body as "{ return e; }", or as "{ e; }" for a void function.
    bool expr_body = false;
    int line = 0, end_line = 0;
    uint32_t pos = 0, end_pos = 0;
    // For a ".cpp" script. When "raw" is true, the translator uses the raw text and not the AST.
    bool raw = false;
    std::string raw_body; // the edited C++ text between the braces, without the braces
    int raw_line = 0; // the script line of the "{": the first line of "raw_body"
    std::vector<RawLine> raw_pre; // statements that come before "raw_body" (the member initializers of a constructor)
};

// A constructor: "Name(T a) : base(args) { body }". The name is the name of the class.
// The base call is optional. Its arguments may use the parameters only (checked by the
// translator). The body runs after the state block has its field initializers.
struct Ctor
{
    std::vector<Param> params;
    Access access = Access::Public;
    bool has_base_call = false;
    std::vector<ExprP> base_args;
    Span base_span; // the raw text between the parentheses of "base(...)"
    BlockStmt body; // the statements; the parser fills them from body_span
    Span body_span; // the raw text between the braces
    int line = 0, end_line = 0;
    uint32_t pos = 0, end_pos = 0, base_pos = 0; // the name, the "}", the "base" word
    // For a ".cpp" script. When "raw" is true, the translator uses the raw text and not the AST.
    bool raw = false;
    std::string raw_body; // the edited C++ text between the braces, without the braces
    int raw_line = 0; // the script line of the "{": the first line of "raw_body"
    std::string raw_base_args; // the text between the parentheses of the base initializer
    size_t raw_base_argc = 0; // the number of arguments in "raw_base_args"
    std::vector<RawLine> raw_pre; // the member initializers as statements, in the order of the fields
};

// A property: "Type Name { get; set; }" or "Type Name { get { ... } set { ... } }".
// An accessor without a body is an auto accessor. An auto property can have "= init;".
// The access on an accessor is parsed and stored. Nothing checks it yet.
struct Property
{
    std::string name, type;
    std::string script_type; // "type" as the script writes it (see Param)
    std::vector<FieldTag> attributes; // the tags in source order (the PropertyInfo table)
    Access access = Access::Public;
    bool has_get = false, has_set = false;
    Access get_access = Access::Public, set_access = Access::Public;
    // A custom accessor has a body. The parser fills the body from the span.
    std::optional<BlockStmt> get_body, set_body;
    Span get_span, set_span; // the raw text between the braces of a custom accessor
    bool get_is_expr = false; // "T Name => e;": get_span is the text of "e"
    ExprP init; // null when the property has no initializer, or the parse failed
    Span init_span; // the raw text after "=", up to the ";"
    bool has_init = false;
    int line = 0;
    uint32_t pos = 0, get_pos = 0, set_pos = 0; // the name, the "get" word, the "set" word
    bool is_static = false; // "static T Name { get; set; }" (gap G23): see ClassDef::sprops
    bool is_auto() const { return !get_body && !set_body; }
};

// "using ns.sub;": the C++ "using namespace ns::sub;". The name is C++ text. A name that starts with
// "std" uses the std placeholder (std_ns.h); the name "std" alone is the text "std".
struct UsingNs
{
    std::string name;
    int line = 0;
    uint32_t pos = 0;
};

// "using Name = type;": a type alias. The "type" is the C++ type text as the parser reads it.
// The parser replaces the name by "resolved" in every type of its scope. "resolved" is "type"
// with the earlier aliases replaced. It has no alias name.
struct TypeAlias
{
    std::string name, type, resolved;
    int line = 0;
    uint32_t pos = 0;
};

struct ClassDef
{
    std::string name, base;
    // A ".cpp" script class in a namespace: "name" is the file-scope name "game__Player", "ns"
    // is the namespace path "game", and "key" is the name that the class record has: "game::Player".
    std::string ns, key;
    const std::string& key_name() const { return key.empty() ? name : key; }
    // The native interface bases after the primary base: "class Boss : Actor, IDamageable"
    // (design 4.4). The primary base is "base".
    std::vector<std::string> interfaces;
    // The script interfaces that the class implements. A later resolution step fills it. The
    // parser leaves it empty.
    std::vector<std::string> script_ifaces;
    std::vector<Field> fields;
    // The class constants ("static const T Name = value;"). The parse step moves them to
    // Script::consts (see Field::owner), so the list is empty after the parse.
    std::vector<Field> consts;
    // The static_assert items of the class body. The parse step moves them to Script::static_asserts.
    std::vector<StaticAssert> static_asserts;
    // The static fields ("static T Name;", gap G23). The parse step moves them to Script::vars
    // (see Field::owner), so the list is empty after the parse.
    std::vector<Field> svars;
    std::vector<Method> methods;
    std::vector<Method> statics; // the "static" methods, in source order
    // The static properties (gap G23), without bodies. The parse step adds the accessors to
    // "statics" as "get_Name" and "set_Name".
    std::vector<Property> sprops;
    std::vector<Ctor> ctors; // the overloads; C++ picks one by the argument types
    std::vector<Method> dtors; // "~Name() { }": the name is "~Name"; the translator allows one
    std::vector<Property> props; // in source order
    std::vector<TypeAlias> aliases; // "using Name = type;" in the class body, in source order
    bool is_value = false; // a struct
    bool is_abstract = false; // "abstract class C { }": the class can have abstract methods
    bool is_private = false; // "private class C { }" at file level: importers do not see the class
    // A body or a field initializer has a "this" that is not the object of a member access. The
    // parser sets it from the text. The owner unit and each importer get the same value.
    bool bare_this = false;
    // The unit of the module that owns a class of an import. The importer has the interface of the
    // class only: no bodies, no state and no record. Calls, fields and "new" go through the runtime
    // helpers (hot::call_script, hot::field, hot::construct). Empty: the class of this module.
    std::string foreign_unit;
    // AOT code: the stem of the file of the owner unit. The importer includes "<foreign_iface>.iface.h".
    std::string foreign_iface;
    // A class of an import that is a script parent of a class of this module (modules step 5). The
    // importer gets the shell and the state struct with the fields, so the child state derives from
    // it. Each body is a forward to the owner unit. The record of the importer is not registered.
    bool iface_parent = false;
    std::string cpp_prelude; // the attribute [cpp_prelude("text")]: C++ text for this class only
    std::string cpp_overrides; // names of virtuals that the shell keeps without a slot (a .cpp script)
    // The script attributes of the class: "[Tag] class C { }". Each name is an attribute class
    // ("[attribute] class Tag;"). The class record keeps the names for a query at run time.
    std::vector<std::string> attributes;
    int line = 0;
    uint32_t pos = 0;
};

// A script interface: "interface IShape { float Area(); }". Each method has a name, a
// parameter list and a return type, and no body. The parser does not set the modifier flags.
struct InterfaceDef
{
    std::string name;
    std::vector<Method> methods;
    int line = 0;
    uint32_t pos = 0;
};

// A class stub: a native class with no body. The attributes give the storage kind.
// The name may end with "<*>" or "::*", or it may be "*". "class : Base;" is a rule for the
// subclasses of Base. "struct *;" carries only a "cpp_prelude".
struct Stub
{
    std::string name;
    bool is_native = false, is_value = false;
    std::string handle, default_handle;
    // [weak_handle("T<$T>")]: the template of a "[weak]" declaration of the handle. Empty: the
    // weak_ptr of a shared_ptr handle.
    std::string weak_handle;
    // [factory("std::make_shared<$T>")]: the callee of "new $T(args)" on a native class. "$T" is the class.
    // Empty: the default of the handle.
    std::string factory;
    // [weak_source("hot::registered_weak_ref<$T>")]: the class is a weak source for pointer fields with
    // no handle. The text is the base of weak_ref_traits<T>. "$T" is the class. Empty: no such source.
    std::string weak_source;
    std::string intrusive_add, intrusive_release;
    // "class : Base;": the rule is for the subclasses of Base only, and "name" is Base.
    bool derived = false;
    bool is_self = false; // the attribute [self]: the rule is for this type only, not subclasses
    bool is_struct = false; // "struct *;": only "cpp_prelude" is valid
    bool checked_index = false; // the attribute [checked_index]: "a[i]" has a bounds test for the type
    // The attribute [nonnull]: a native call that returns the type gives a non null value. On "class *"
    // it is the default of the library.
    bool nonnull = false;
    std::string cpp_prelude; // the attribute [cpp_prelude("text")]
    // The attribute [cpp_overrides("A B")]: the names of the native virtuals that the cpp_prelude
    // text overrides. The shells get no slot and no stub for them.
    std::string cpp_overrides;
    // "[attribute] class Name;": a script attribute class, not a native type. A script class
    // uses it as a tag "[Name]". It has no storage rule.
    bool is_attribute = false;
    // The names of the attributes that the stub states. A later stub with the same key merges
    // only these attributes into the rule (TypeModel::add_stubs).
    std::vector<std::string> attrs;
    int line = 0;
    uint32_t pos = 0;
};

// A module attribute list of a host: "[std("eastl")];". An attribute list with ";" and no item.
// Any .hot file may hold it. A list in a prelude is the default of the host. A list in a script
// overrides the default for that script. An attribute name is valid once in one file. Each
// attribute sets one field. A later attribute adds a field and a row in the attribute table of the parser
// ("module_attr_arity" in script_grammar.cpp).
struct ModuleAttrs
{
    bool has_std = false; // the attribute [std("name")]
    std::string std_ns = "std"; // the namespace of the standard library names
    std::vector<std::pair<std::string, uint32_t>> used; // the attribute names and their positions
    int line = 0;
    uint32_t pos = 0;
    uint32_t std_pos = 0; // the position of the attribute "std"
    bool has_array = false; // the attribute [array("template")]
    // The template of the type "T[N]": "$T" is the element type and "$N" is the size. It is the
    // internal form: the placeholder of std_ns.h stands for "$std::".
    std::string array_tpl = std::string(kDefaultArrayTpl);
    uint32_t array_pos = 0; // the position of the attribute "array"
    // The attributes [lib("name")]: the standard-like library namespaces, for example "eastl". A
    // script names their types with the namespace ("eastl.vector<int>"), next to "std.vector<int>".
    std::vector<std::string> libs;
};

// A script enum: "enum Name : type { A, B = 5 }". It becomes a C++ "enum class". The
// underlying type is a builtin integer type; empty means int32_t. A value is optional.
struct Enumerator
{
    std::string name;
    ExprP value; // null when the enumerator has no value, or the parse failed
    Span value_span; // the raw text after "="
    bool has_value = false;
    int line = 0;
    uint32_t pos = 0;
};

struct EnumDef
{
    std::string name, underlying; // underlying: C++ type text, or empty
    std::vector<Enumerator> items;
    bool is_private = false; // "private enum E { }": importers do not see the enum
    int line = 0;
    uint32_t pos = 0, underlying_pos = 0;
};

struct Import
{
    std::string path; // the header name; a system header keeps its angle brackets
    int line;
    uint32_t pos = 0;
    // The text after #include: <vector> for a system header, "Actor.h" for the others.
    std::string target() const { return !path.empty() && path.front() == '<' ? path : "\"" + path + "\""; }
};

struct Script
{
    std::vector<Import> imports;
    std::vector<UsingNs> using_ns; // in source order
    std::vector<TypeAlias> aliases; // module level, in source order
    std::vector<ClassDef> classes;
    std::vector<InterfaceDef> interfaces; // in source order
    std::vector<Stub> stubs; // in source order
    // The script attribute classes: "[attribute] class Name;". They are not in "stubs".
    std::vector<Stub> attribute_classes;
    std::vector<ModuleAttrs> module_attrs; // the module attribute lists of the file, in source order
    std::vector<EnumDef> enums;
    // Module variables: "Type name = init;" at file level. The translator collects them in
    // the module state block.
    std::vector<Field> vars;
    // Module constants: "const Type name = value;" at file level. They are C++ "static const" values,
    // not parts of the module state block. An imported script can have them.
    std::vector<Field> consts;
    // The static_assert items at file level and in class bodies, in source order.
    std::vector<StaticAssert> static_asserts;
    // Convert hooks of module variables: "T convert_<var>(Old old) { }" at file level.
    std::vector<Method> var_hooks;
    // Free functions: "T name(args) { }" at file level. A convert hook is not one of them.
    std::vector<Method> fns;
    // True when the script is a ".cpp" file. The bodies are then raw C++ text.
    bool is_cpp = false;
    // One part of the output of a ".cpp" script. The translator writes the chunks in source order.
    struct Chunk
    {
        enum class Kind
        {
            Text, // a file-scope declaration or an include, with the edits of a body
            Class, // the generated block of the script class "name"
            Module // the module state block, at the first module variable
        };
        Kind kind = Kind::Text;
        int line = 0;
        uint32_t pos = 0;
        std::string text; // Text only
        std::string name; // Class only
        // The namespace path of a Text chunk ("game::ai"). The translator writes the text in it.
        std::string ns;
    };
    std::vector<Chunk> chunks; // a ".cpp" script only
};

} // namespace hotc::ast
