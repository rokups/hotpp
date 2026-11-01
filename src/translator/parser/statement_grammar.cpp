// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The grammar of the statements.
//
// The grammar is one lexy parse over the text of a block. It builds the Stmt nodes of ast.h.
// A child construct of another grammar works with "span then call": this grammar scans the
// span of an expression and calls parse_expression or parse_expression_list. The expression
// grammar calls parse_block for the body of a lambda. All parsing uses the same text buffer,
// so each position is an absolute byte offset.
//
// Rules:
//  - A keyword uses LEXY_KEYWORD. So "ifx" and "letter" are names, not keywords.
//    The words "var", "foreach", "in", "using", "override", "virtual", "new", "public",
//    "private", "protected", "get", "set", "finally" and "void" cannot be names.
//  - An expression span ends at a terminator at depth 0. The depth counts (), [] and {}.
//    Strings, characters and comments are skipped.
//  - The label of a "case" ends at the first ":" at depth 0. The script has no "::".
//    A bare ternary "a ? b : c" in a label is not supported, because its ":" ends the
//    label. Put the ternary in parentheses.
//  - A catch clause is "catch (Type name)" or "catch (...)". A clause with a type and no
//    name is an error.
//  - A try statement has one or more catch clauses, a "finally" block, or both.
//  - A local declaration is "Type name", "var name", "var& name", "const Type name",
//    "const var name" or "const var& name". The "var&" form is a local reference.
//    The look-ahead rule for "Type name" is in the comment above DeclEnd.
//  - A foreach statement is "foreach ( (const)? (var &? | Type) name in expr ) stmt".
//    Other for statements are classic.
//  - The init of a classic for is one declaration or a list of expressions.
//
// Error recovery: a broken statement skips to the next ";" (consumed) or to the "}" that
// ends the block (not consumed). The next statements still parse. A statement with an
// error in an expression is not in the result. The error is in the diagnostics.
//
// Nesting: lexy limits the nesting of statements and brackets inside one block. A
// thread-local counter limits the nesting of parse_block calls (lambda in lambda).
#include "expression.h"
#include "lexy_common.hpp"
#include "statement.h"

#include <cctype>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace hotc
{
namespace
{

using namespace parser; // NOLINT: the grammar uses the names of lexy_common.hpp
using ast::ExprP;
using ast::StmtP;

// ---------------------------------------------------------------- limits
// The limit of dsl::recurse in one block: nested statements and brackets.
constexpr std::size_t max_depth = 64;
// The limit of nested parse_block calls: a lambda in a lambda.
constexpr int max_block_nesting = 16;

// ---------------------------------------------------------------- error texts
HOT_ERROR_TAG(e_stmt, "expected a statement");
HOT_ERROR_TAG(e_while, "expected 'while'");
HOT_ERROR_TAG(e_name, "expected a name");
HOT_ERROR_TAG(e_catch, "expected 'catch' or 'finally'");
HOT_ERROR_TAG(e_const_form, "expected 'var' or a type after 'const'");

// ---------------------------------------------------------------- words
constexpr auto kw_const = LEXY_KEYWORD("const", id);
constexpr auto kw_if = LEXY_KEYWORD("if", id);
constexpr auto kw_else = LEXY_KEYWORD("else", id);
constexpr auto kw_while = LEXY_KEYWORD("while", id);
constexpr auto kw_do = LEXY_KEYWORD("do", id);
constexpr auto kw_for = LEXY_KEYWORD("for", id);
constexpr auto kw_return = LEXY_KEYWORD("return", id);
constexpr auto kw_break = LEXY_KEYWORD("break", id);
constexpr auto kw_continue = LEXY_KEYWORD("continue", id);
constexpr auto kw_switch = LEXY_KEYWORD("switch", id);
constexpr auto kw_case = LEXY_KEYWORD("case", id);
constexpr auto kw_default = LEXY_KEYWORD("default", id);
constexpr auto kw_try = LEXY_KEYWORD("try", id);
constexpr auto kw_catch = LEXY_KEYWORD("catch", id);
constexpr auto kw_throw = LEXY_KEYWORD("throw", id);

// A name. The words of the body syntax and the reserved words are not names.
constexpr auto id_r = id.reserve(kw_const, kw_if, kw_else, kw_while, kw_do, kw_for, kw_return, kw_break, kw_continue,
    kw_switch, kw_case, kw_default, kw_try, kw_catch, kw_throw, kw_var, kw_foreach, kw_in, kw_using, kw_override,
    kw_virtual, kw_async, kw_await, kw_new, kw_public, kw_private, kw_protected, kw_get, kw_set, kw_finally,
    kw_void);

constexpr auto var_or_const = dsl::literal_set(kw_var, kw_const);

// ---------------------------------------------------------------- plain helpers
std::string_view span_text(const ParseState& st, Span sp)
{
    return std::string_view(st.base + sp.begin, sp.end - sp.begin);
}

bool blank(const ParseState& st, Span sp)
{
    return only_blank(span_text(st, sp));
}

// The type text of a span. A "." is a scope and becomes "::". A "::" is an error.
std::string type_of(const ParseState& st, Span sp)
{
    return type_of_span(st, sp);
}

ParseInput input_of(const ParseState& st)
{
    return ParseInput{std::string_view(st.base, st.size), st.map, st.array_tpl};
}

// The expression of the span. Null: the error is in the diagnostics.
ExprP expr_of(const ParseState& st, Span sp)
{
    return parse_expression(input_of(st), sp, *st.diag);
}

// The expression list of the span. False: the error is in the diagnostics.
bool list_of(const ParseState& st, Span sp, std::vector<ExprP>& out)
{
    if (blank(st, sp))
        return true;
    const std::size_t before = st.diag->items.size();
    out = parse_expression_list(input_of(st), sp, *st.diag);
    return st.diag->items.size() == before;
}

// ---------------------------------------------------------------- values
struct VarHead
{
    uint32_t pos = 0;
    uint32_t name_pos = 0; // the byte offset of the name
    bool is_const = false;
    bool is_ref = false; // "var& name"
    std::string name, type;
    std::vector<std::string> names; // "var (a, b)": a structured binding, "name" is empty
    std::vector<uint32_t> name_poss;
};

// The names after "var": one name, or the names of a structured binding "(a, b)".
struct NameList
{
    std::vector<std::string> names;
    std::vector<uint32_t> poss;
    bool binding = false;
};

// A binding needs two or more names and an initializer. The error is in the diagnostics.
bool binding_ok(const ParseState& st, uint32_t pos, size_t count)
{
    if (count >= 2)
        return true;
    st.error(st.base + pos, "a structured binding needs two or more names");
    return false;
}

struct DeclParts
{
    VarHead head;
    bool has_init = false;
    Span init;
};

// One attribute before a local declaration: "[weak] T x = e;". The name and its byte offset.
using StmtAttr = std::pair<std::string, uint32_t>;

struct ForInit
{
    bool is_decl = false;
    DeclParts decl;
    Span span; // the expressions, when is_decl is false
};

struct ForRest
{
    ForInit init;
    Span cond, step;
    StmtP body;
};

struct ElsePart
{
    uint32_t pos = 0;
    StmtP stmt;
};

struct SwItem
{
    enum Kind
    {
        None,
        Case,
        Default,
        Statement
    } kind = None;
    ExprP label; // Case: null when the label has an error
    uint32_t pos = 0;
    StmtP stmt;
};

StmtP make_decl(const ParseState& st, DeclParts&& d)
{
    ast::VarDeclStmt v;
    v.is_const = d.head.is_const;
    v.is_ref = d.head.is_ref;
    v.name = std::move(d.head.name);
    v.type = std::move(d.head.type);
    v.names = std::move(d.head.names);
    v.name_poss = std::move(d.head.name_poss);
    v.pos = d.head.pos;
    v.name_pos = d.head.name_pos;
    if (!v.names.empty())
    {
        if (!binding_ok(st, v.pos, v.names.size()))
            return nullptr;
        if (!d.has_init)
        {
            st.error(st.base + v.pos, "a structured binding needs an initializer");
            return nullptr;
        }
    }
    // "Type name;" is value-initialized by the emitter. "var" and "const" need an initializer.
    if (!d.has_init)
    {
        if (v.type.empty())
            st.error(st.base + v.pos, "var needs an initializer");
        else if (v.is_const)
            st.error(st.base + v.pos, "a const needs an initializer");
    }
    if (d.has_init)
    {
        v.init = expr_of(st, d.init);
        if (!v.init)
            return nullptr;
    }
    return ast::make_stmt(std::move(v));
}

// A local declaration with attributes: "[weak] T x = e;". Only "weak" is known. A weak local
// needs a written handle type, so "var", "var&" and a structured binding are errors.
StmtP make_attr_decl(const ParseState& st, std::vector<StmtAttr>&& attrs, DeclParts&& d)
{
    uint32_t weak_pos = 0;
    for (const StmtAttr& a : attrs)
    {
        if (a.first != "weak")
            st.error(st.base + a.second, "unknown attribute '" + a.first + "' on a local variable");
        else if (weak_pos != 0)
            st.error(st.base + a.second, "duplicate attribute 'weak'");
        else
            weak_pos = a.second;
    }
    const bool untyped = d.head.type.empty();
    StmtP s = make_decl(st, std::move(d));
    if (!s || weak_pos == 0)
        return s;
    auto* v = std::get_if<ast::VarDeclStmt>(&s->v);
    if (!v)
        return s;
    if (untyped)
    {
        st.error(st.base + weak_pos,
            "[weak] needs the handle type of the local: write '[weak] T name', not 'var' or a structured binding");
        return nullptr;
    }
    v->weak = true;
    v->weak_pos = weak_pos;
    return s;
}

std::vector<StmtP> keep(std::vector<StmtP>&& v)
{
    std::vector<StmtP> out;
    for (StmtP& s : v)
        if (s)
            out.push_back(std::move(s));
    return out;
}

// ---------------------------------------------------------------- tokens
// A name. The value is the text.
struct name_p : lexy::token_production
{
    static constexpr auto rule = dsl::must(id_r).template error<e_name>;
    static constexpr auto value = lexy::as_string<std::string>;
};

// A name of a structured binding, with its byte offset.
struct binding_name
{
    static constexpr auto rule = dsl::position + dsl::p<name_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<std::pair<std::string, uint32_t>>([](const ParseState& st, const char* pos, std::string name)
    { return std::make_pair(std::move(name), st.offset(pos)); }),
        lexy::parse_state, lexy::values);
};

// "(a, b, ...)": the names of a structured binding.
struct binding_names
{
    static constexpr auto rule =
        dsl::lit_c<'('> + dsl::list(dsl::p<binding_name>, dsl::sep(dsl::lit_c<','>)) + dsl::lit_c<')'>;
    static constexpr auto value = lexy::as_list<std::vector<std::pair<std::string, uint32_t>>>;
};

// The name after "var": "name" or "(a, b)".
struct decl_names
{
    static constexpr auto rule = dsl::peek(dsl::lit_c<'('>) >> dsl::p<binding_names> | dsl::else_ >> dsl::p<name_p>;
    static constexpr auto value = lexy::callback<NameList>([](std::string&& name)
    { return NameList{{std::move(name)}, {}, false}; }, [](std::vector<std::pair<std::string, uint32_t>>&& list)
    {
        NameList n;
        n.binding = true;
        for (auto& [name, pos] : list)
        {
            n.names.push_back(std::move(name));
            n.poss.push_back(pos);
        }
        return n;
    });
};

// ---------------------------------------------------------------- terminators
constexpr auto end_of_stmt = dsl::eof | (dsl::lit_c<';'> / dsl::lit_c<'}'>);
constexpr auto end_of_part = dsl::eof | (dsl::lit_c<';'> / dsl::lit_c<')'> / dsl::lit_c<'}'>);

// The expression of a statement.
struct expr_cfg
{
    static constexpr auto open = dsl::whitespace(ws_rule);
    static constexpr auto term = end_of_stmt;
};
// The "= expression" of a declaration.
struct init_cfg
{
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = end_of_stmt;
};
// The "( expression" of if, while, do-while and switch. The ")" stays in the input.
struct cond_cfg
{
    static constexpr auto open = dsl::lit_c<'('>;
    static constexpr auto term = end_of_part;
};
// A part of the header of a for statement.
struct part_cfg
{
    static constexpr auto open = dsl::whitespace(ws_rule);
    static constexpr auto term = end_of_part;
};
struct for_init_cfg
{
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = end_of_part;
};

// ---------------------------------------------------------------- declarations
// A local declaration has one of these forms:
//   Type name = e;   Type name;   var name = e;   const Type name = e;   const var name = e;
// "var" gives an empty type. The C++ text then has "auto".
//
// The look-ahead rule for "Type name": the statement is a declaration when a scan of a type
// at depth zero ends at an identifier, and a "=" (not "==") or a ";" follows that identifier.
// Otherwise the statement is an expression statement. The rule is the C++ rule:
//  - "a * b = c;" and "a * b;" declare "b" with the type "a *". "a & b;" is the same.
//  - "f(x);", "a = b;", "a.b = c;", "x++;" and "*p = 1;" are expressions.
//  - "std.string s;" is a declaration. A "." (or a "::") joins the names of a qualified name.
//    An identifier after a "." is part of the type and is never the declared name.
// The type starts with an identifier that is not a keyword. At depth zero the type has only
// identifiers, ".", "::", "*", "&", "&&", "<...>" and "[...]". Any other token, for example
// "(", "+", "=" or "?", ends the scan: the statement is an expression.
// Templates: a "<" after a part of the type opens an argument list. The scan counts "<>",
// "()" and "[]". A ">>" is two closers. A ";", a "{", a "}" or the end of the text inside
// the list means: not a declaration. So "a < b > c;" declares "c" with the type "a<b>", as
// in C++. "a < b;" has no ">", so it stays an expression. Write "(a < b) > c;" to compare.
// In the head of a foreach statement, the keyword "in" follows the name, not "=" or ";".
enum class DeclEnd
{
    Stmt,
    ForEach
};

// A cursor over a copy of a lexy reader. A char is a value 0..255. The end of the text is -1.
template <typename Reader> class DeclCursor
{
public:
    explicit DeclCursor(Reader r)
        : r_(r)
    {
    }
    int get(int k = 0) const
    {
        Reader c = r_;
        for (int i = 0; i < k; ++i)
        {
            if (c.peek() == Reader::encoding::eof())
                return -1;
            c.bump();
        }
        return c.peek() == Reader::encoding::eof() ? -1 : static_cast<int>(c.peek());
    }
    void bump(int n = 1)
    {
        for (int i = 0; i < n && r_.peek() != Reader::encoding::eof(); ++i)
            r_.bump();
    }
    typename Reader::marker marker() const { return r_.current(); }

    // Skips white space and comments.
    void skip_blank()
    {
        for (;;)
        {
            const int c = get();
            if (c >= 0 && std::isspace(c))
            {
                bump();
            }
            else if (c == '/' && get(1) == '/')
            {
                while (get() >= 0 && get() != '\n')
                    bump();
            }
            else if (c == '/' && get(1) == '*')
            {
                bump(2);
                while (get() >= 0 && !(get() == '*' && get(1) == '/'))
                    bump();
                bump(2);
            }
            else
            {
                return;
            }
        }
    }
    bool at_id0() const
    {
        const int c = get();
        return c >= 0 && is_id0(static_cast<char>(c));
    }
    // Reads an identifier. The cursor is at the first char of it.
    std::string word()
    {
        std::string w;
        while (get() >= 0 && is_id(static_cast<char>(get())))
        {
            w += static_cast<char>(get());
            bump();
        }
        return w;
    }
    // Skips a string or a character literal. False at the end of the text.
    bool skip_literal()
    {
        const int q = get();
        bump();
        while (get() >= 0 && get() != q)
        {
            if (get() == '\\')
                bump();
            bump();
        }
        if (get() < 0)
            return false;
        bump();
        return true;
    }

private:
    Reader r_;
};

// The words that cannot start a type: the keywords of statements and expressions.
inline bool is_decl_keyword(const std::string& w)
{
    static const char* const words[] = {"if", "else", "while", "do", "for", "return", "break", "continue", "switch",
        "case", "default", "try", "catch", "throw", "var", "foreach", "in", "const", "this", "base", "null", "new",
        "true", "false", "delete", "async", "await", nullptr};
    for (int i = 0; words[i]; ++i)
        if (w == words[i])
            return true;
    return false;
}

// Skips a bracket list of a type: "<...>" or "[...]". The cursor is at the opener.
// False when a ";", a "{", a "}", a wrong closer or the end of the text comes first.
template <typename Reader> bool skip_type_list(DeclCursor<Reader>& c)
{
    std::string stack(1, static_cast<char>(c.get()));
    c.bump();
    while (!stack.empty())
    {
        c.skip_blank();
        const int ch = c.get();
        const char top = stack.back();
        if (ch < 0 || ch == ';' || ch == '{' || ch == '}')
            return false;
        if (ch == '"' || ch == '\'')
        {
            if (!c.skip_literal())
                return false;
            continue;
        }
        if (ch == '-' && c.get(1) == '>')
        {
            c.bump(2);
            continue;
        }
        if ((ch == '<' || ch == '>') && c.get(1) == '=')
        {
            c.bump(2);
            continue;
        }
        c.bump();
        if (ch == '(' || ch == '[' || (ch == '<' && top == '<'))
        {
            stack += static_cast<char>(ch);
        }
        else if (ch == ')' || ch == ']' || (ch == '>' && top == '<'))
        {
            const char open = ch == ')' ? '(' : ch == ']' ? '[' : '<';
            if (top != open)
                return false;
            stack.pop_back();
        }
    }
    return true;
}

// The look-ahead of "Type name". The rule is in the comment above DeclEnd.
// True when the text at the cursor is a type and a name. *name is the position of the name.
template <DeclEnd E, typename Reader> bool find_decl_type(DeclCursor<Reader> c, typename Reader::marker* name)
{
    bool have_type = false; // the scan has one part of the type or more
    bool after_dot = false; // the last token is a "." or a "::"
    for (;;)
    {
        c.skip_blank();
        const int ch = c.get();
        if (c.at_id0())
        {
            const typename Reader::marker at = c.marker();
            const std::string w = c.word();
            if (!have_type && is_decl_keyword(w))
                return false;
            if (have_type && !after_dot)
            {
                DeclCursor<Reader> next = c;
                next.skip_blank();
                const int n = next.get();
                bool end = false;
                if constexpr (E == DeclEnd::Stmt)
                {
                    end = n == ';' || (n == '=' && next.get(1) != '=');
                }
                else
                {
                    end = next.at_id0() && next.word() == "in";
                }
                if (end)
                {
                    *name = at;
                    return true;
                }
            }
            have_type = true;
            after_dot = false;
            continue;
        }
        if (!have_type)
            return false;
        if (ch == '.' && c.get(1) != '.')
        {
            c.bump();
            after_dot = true;
        }
        else if (ch == ':' && c.get(1) == ':')
        {
            c.bump(2);
            after_dot = true;
        }
        else if ((ch == '*' || ch == '&') && c.get(1) != '=')
        {
            c.bump(ch == '&' && c.get(1) == '&' ? 2 : 1);
            after_dot = false;
        }
        else if ((ch == '<' && c.get(1) != '<' && c.get(1) != '=') || ch == '[')
        {
            if (!skip_type_list(c))
                return false;
            after_dot = false;
        }
        else
        {
            return false;
        }
    }
}

struct probe_decl_stmt
{
    template <typename Reader> static bool run(Reader r, typename Reader::marker* name)
    {
        return find_decl_type<DeclEnd::Stmt>(DeclCursor<Reader>(r), name);
    }
};
struct probe_decl_foreach
{
    template <typename Reader> static bool run(Reader r, typename Reader::marker* name)
    {
        return find_decl_type<DeclEnd::ForEach>(DeclCursor<Reader>(r), name);
    }
};
HOT_ERROR_TAG(e_decl_type, "expected a declaration");

// A branch rule over a plain C++ look-ahead. Probe::run decides the branch.
// Consume false: the rule is a peek. It consumes no text and produces no value.
// Consume true: the rule consumes the type and produces two positions: the begin of the type
// and the begin of the name. The name stays in the input.
template <typename Probe, bool Consume> struct probe_rule : dsl::branch_base
{
    template <typename Reader> struct bp
    {
        typename Reader::iterator begin{};
        typename Reader::marker name{};

        constexpr bool try_parse(const void*, const Reader& reader)
        {
            begin = reader.position();
            name = reader.current();
            return Probe::run(reader, &name);
        }

        template <typename Context> constexpr void cancel(Context&) {}

        template <typename NextParser, typename Context, typename... Args>
        LEXY_PARSER_FUNC bool finish(Context& context, Reader& reader, Args&&... args)
        {
            if constexpr (Consume)
            {
                reader.reset(name);
                return NextParser::parse(context, reader, LEXY_FWD(args)..., begin, name.position());
            }
            else
            {
                return NextParser::parse(context, reader, LEXY_FWD(args)...);
            }
        }
    };

    template <typename NextParser> struct p
    {
        template <typename Context, typename Reader, typename... Args>
        LEXY_PARSER_FUNC static bool parse(Context& context, Reader& reader, Args&&... args)
        {
            bp<Reader> impl{};
            if (!impl.try_parse(context.control_block, reader))
            {
                auto err = lexy::error<Reader, e_decl_type>(reader.position());
                context.on(lexy::parse_events::error{}, err);
                return false;
            }
            return impl.template finish<NextParser>(context, reader, LEXY_FWD(args)...);
        }
    };
};

// The peek of "Type name" in a statement and in the init of a for statement.
constexpr auto decl_ahead = probe_rule<probe_decl_stmt, false>{};
// The type of "Type name". It produces the begin of the type and the begin of the name.
template <DeclEnd E>
constexpr auto decl_type =
    probe_rule<std::conditional_t<E == DeclEnd::Stmt, probe_decl_stmt, probe_decl_foreach>, true>{};

VarHead make_head(const ParseState& st, const char* pos, std::string name)
{
    VarHead h;
    h.pos = st.offset(pos);
    h.name = std::move(name);
    return h;
}

VarHead make_typed_head(
    const ParseState& st, const char* pos, const char* type_begin, const char* type_end, std::string name)
{
    VarHead h = make_head(st, pos, std::move(name));
    h.type = type_of(st, Span{st.offset(type_begin), st.offset(type_end)});
    return h;
}

// Puts the names of a "var" head: one name, or the names of a structured binding.
void set_names(VarHead& h, NameList&& n)
{
    if (n.binding)
    {
        h.names = std::move(n.names);
        h.name_poss = std::move(n.poss);
    }
    else
    {
        h.name = std::move(n.names[0]);
    }
}

// The optional "&" after "var": a local reference. The value is true when the "&" is there.
struct ref_mark
{
    static constexpr auto rule = dsl::opt(dsl::capture(dsl::lit_c<'&'>));
    static constexpr auto value =
        lexy::callback<bool>([](lexy::nullopt) { return false; }, [](auto&&) { return true; });
};

// The part after "const": "var name", "var& name" or "Type name".
template <DeclEnd E> struct const_rest
{
    static constexpr auto rule = kw_var >> dsl::p<ref_mark> + dsl::p<decl_names> | decl_type<E> >> dsl::p<name_p>
        | dsl::else_ >> dsl::error<e_const_form>;
    static constexpr auto value = lexy::bind(lexy::callback<VarHead>(
                                                 [](const ParseState& st, bool is_ref, NameList&& n)
    {
        VarHead h = make_head(st, st.base, std::string());
        h.is_ref = is_ref;
        set_names(h, std::move(n));
        return h;
    }, [](const ParseState& st, const char* b, const char* e, std::string name)
    { return make_typed_head(st, st.base, b, e, std::move(name)); }),
        lexy::parse_state, lexy::values);
};

// "var name", "const var name", "const Type name" or "Type name".
template <DeclEnd E> struct var_head
{
    static constexpr auto rule = dsl::position
        + (kw_var >> dsl::p<ref_mark> + dsl::p<decl_names> | kw_const >> dsl::p<const_rest<E>>
            | decl_type<E> >> dsl::p<name_p> | dsl::else_ >> dsl::error<e_decl_type>)+dsl::position;
    // The last argument is the end of the name. White space can come before it, so the name
    // starts "name.size()" bytes before the end, after the white space is removed.
    static uint32_t name_at(const ParseState& st, const char* pos, const char* end, size_t len)
    {
        while (end > pos && std::isspace(static_cast<unsigned char>(end[-1])))
            --end;
        return st.offset(end) - static_cast<uint32_t>(len);
    }
    static constexpr auto value = lexy::bind(
        lexy::callback<VarHead>(
            [](const ParseState& st, const char* pos, bool is_ref, NameList&& n, const char* end)
    {
        VarHead h = make_head(st, pos, std::string());
        h.is_ref = is_ref;
        set_names(h, std::move(n));
        if (h.names.empty())
            h.name_pos = name_at(st, pos, end, h.name.size());
        else
            h.name_pos = h.name_poss[0];
        return h;
    },
            [](const ParseState& st, const char* pos, VarHead&& h, const char* end)
    {
        h.pos = st.offset(pos);
        h.is_const = true;
        if (h.names.empty())
            h.name_pos = name_at(st, pos, end, h.name.size());
        else
            h.name_pos = h.name_poss[0];
        return std::move(h);
    },
            [](const ParseState& st, const char* pos, const char* b, const char* e, std::string name, const char* end)
    {
        VarHead h = make_typed_head(st, pos, b, e, std::move(name));
        h.name_pos = name_at(st, pos, end, h.name.size());
        return h;
    }),
        lexy::parse_state, lexy::values);
};

// A declaration without the ";".
template <typename InitCfg> struct decl_p
{
    static constexpr auto rule =
        dsl::p<var_head<DeclEnd::Stmt>> + dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<InitCfg>>);
    static constexpr auto value = lexy::callback<DeclParts>([](VarHead&& h, lexy::nullopt) {
        return DeclParts{std::move(h), false, {}};
    }, [](VarHead&& h, Span init) { return DeclParts{std::move(h), true, init}; });
};

constexpr auto semi = dsl::try_(dsl::lit_c<';'>);

struct stmt;

struct decl_stmt
{
    static constexpr auto rule = dsl::p<decl_p<init_cfg>> + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, DeclParts&& d) { return make_decl(st, std::move(d)); }),
        lexy::parse_state, lexy::values);
};

// One attribute name of a local declaration, with its byte offset.
struct stmt_attr_p
{
    static constexpr auto rule = dsl::position + dsl::p<name_p>;
    static constexpr auto value =
        lexy::bind(lexy::callback<StmtAttr>([](const ParseState& st, const char* pos, std::string name)
    { return StmtAttr{std::move(name), st.offset(pos)}; }),
            lexy::parse_state, lexy::values);
};

struct stmt_attr_list
{
    static constexpr auto rule = dsl::terminator(dsl::lit_c<']'>).list(dsl::p<stmt_attr_p>, dsl::sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<StmtAttr>>;
};

// "[weak] T x = e;": a local declaration with attributes.
struct attr_decl_stmt
{
    static constexpr auto rule = dsl::lit_c<'['> + dsl::p<stmt_attr_list> + dsl::p<decl_p<init_cfg>> + semi;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>([](const ParseState& st,
                                                                       std::vector<StmtAttr>&& attrs, DeclParts&& d)
    { return make_attr_decl(st, std::move(attrs), std::move(d)); }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- simple statements
struct expr_stmt
{
    static constexpr auto rule = dsl::position + dsl::p<scan_until<expr_cfg>> + semi;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>(
                                                 [](const ParseState& st, const char* pos, Span sp) -> StmtP
    {
        ExprP e = expr_of(st, sp);
        if (!e)
            return nullptr;
        return ast::make_stmt(ast::ExprStmt{std::move(e), st.offset(pos)});
    }),
        lexy::parse_state, lexy::values);
};

struct return_stmt
{
    static constexpr auto rule = dsl::position + kw_return + dsl::p<scan_until<expr_cfg>> + semi;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>(
                                                 [](const ParseState& st, const char* pos, Span sp) -> StmtP
    {
        ast::ReturnStmt r;
        r.pos = st.offset(pos);
        if (!blank(st, sp))
        {
            r.expr = expr_of(st, sp);
            if (!r.expr)
                return nullptr;
        }
        return ast::make_stmt(std::move(r));
    }),
        lexy::parse_state, lexy::values);
};

struct throw_stmt
{
    static constexpr auto rule = dsl::position + kw_throw + dsl::p<scan_until<expr_cfg>> + semi;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>(
                                                 [](const ParseState& st, const char* pos, Span sp) -> StmtP
    {
        ast::ThrowStmt t;
        t.pos = st.offset(pos);
        if (!blank(st, sp))
        {
            t.expr = expr_of(st, sp);
            if (!t.expr)
                return nullptr;
        }
        return ast::make_stmt(std::move(t));
    }),
        lexy::parse_state, lexy::values);
};

struct break_stmt
{
    static constexpr auto rule = dsl::position + kw_break + semi;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>([](const ParseState& st, const char* pos)
    { return ast::make_stmt(ast::BreakStmt{st.offset(pos)}); }),
        lexy::parse_state, lexy::values);
};

struct continue_stmt
{
    static constexpr auto rule = dsl::position + kw_continue + semi;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>([](const ParseState& st, const char* pos)
    { return ast::make_stmt(ast::ContinueStmt{st.offset(pos)}); }),
        lexy::parse_state, lexy::values);
};

struct empty_stmt
{
    static constexpr auto rule = dsl::position + dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>([](const ParseState& st, const char* pos)
    { return ast::make_stmt(ast::EmptyStmt{st.offset(pos)}); }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- recovery
// The end of the list of statements. The top list ends at the end of the text. A "}" in it
// is a stray char. A list in a block ends before the "}".
struct top_cfg
{
    static constexpr auto closer = dsl::eof;
    static constexpr auto stop = dsl::lit_c<'}'> >> dsl::break_;
};
struct inner_cfg
{
    static constexpr auto closer = dsl::eof | dsl::lit_c<'}'>;
    static constexpr auto stop = dsl::peek(dsl::lit_c<'}'>) >> dsl::break_;
};

// Skips the rest of a broken statement. It stops after a ";" and at the end of the text.
// It stops before the "}" of a block. A nested block, a string and a comment are skipped.
// A stray "}" in the top list is skipped, and the sync stops after it.
template <typename Cfg, typename T> struct sync : lexy::token_production
{
    static constexpr auto rule = dsl::loop(
        dsl::eof >> dsl::break_ | dsl::lit_c<';'> >> dsl::break_ | Cfg::stop | dsl::else_ >> dsl::p<skip_item>);
    static constexpr auto value = lexy::callback<T>([]() { return T{}; });
};

// ---------------------------------------------------------------- blocks
// The statements of a block: a list with nulls for the broken statements.
template <typename Cfg> struct stmts_p
{
    static constexpr auto item = dsl::peek_not(Cfg::closer) >> dsl::try_(dsl::recurse<stmt>, dsl::p<sync<Cfg, StmtP>>);
    static constexpr auto rule = dsl::opt(dsl::peek_not(Cfg::closer) >> dsl::list(item));
    static constexpr auto value = lexy::as_list<std::vector<StmtP>>;
};

struct block_p
{
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'{'> + dsl::p<stmts_p<inner_cfg>> + dsl::position + dsl::lit_c<'}'>;
    static constexpr auto value =
        lexy::bind(lexy::callback<StmtP>(
                       [](const ParseState& st, const char* b, std::vector<StmtP>&& v, const char* e)
    {
        ast::BlockStmt blk;
        blk.stmts = keep(std::move(v));
        blk.pos = st.offset(b);
        blk.end_pos = st.offset(e);
        return ast::make_stmt(std::move(blk));
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- if, while, do
struct else_part
{
    static constexpr auto rule = dsl::position + kw_else + dsl::recurse<stmt>;
    static constexpr auto value =
        lexy::bind(lexy::callback<ElsePart>([](const ParseState& st, const char* pos, StmtP&& s)
    { return ElsePart{st.offset(pos), std::move(s)}; }),
            lexy::parse_state, lexy::values);
};

struct if_stmt
{
    static constexpr auto rule = dsl::position + kw_if + dsl::p<scan_until<cond_cfg>> + dsl::lit_c<')'>
        + dsl::recurse<stmt> + dsl::opt(dsl::peek(kw_else) >> dsl::p<else_part>);
    static constexpr auto value =
        lexy::bind(lexy::callback<StmtP>(
                       [](const ParseState& st, const char* pos, Span cond, StmtP&& then, auto&& other) -> StmtP
    {
        ast::IfStmt n;
        n.pos = st.offset(pos);
        n.cond = expr_of(st, cond);
        n.then = std::move(then);
        if constexpr (!std::is_same_v<std::decay_t<decltype(other)>, lexy::nullopt>)
        {
            n.else_pos = other.pos;
            n.otherwise = std::move(other.stmt);
            if (!n.otherwise)
                return nullptr;
        }
        if (!n.cond || !n.then)
            return nullptr;
        return ast::make_stmt(std::move(n));
    }),
            lexy::parse_state, lexy::values);
};

struct while_stmt
{
    static constexpr auto rule =
        dsl::position + kw_while + dsl::p<scan_until<cond_cfg>> + dsl::lit_c<')'> + dsl::recurse<stmt>;
    static constexpr auto value =
        lexy::bind(lexy::callback<StmtP>(
                       [](const ParseState& st, const char* pos, Span cond, StmtP&& body) -> StmtP
    {
        ast::WhileStmt n;
        n.pos = st.offset(pos);
        n.cond = expr_of(st, cond);
        n.body = std::move(body);
        if (!n.cond || !n.body)
            return nullptr;
        return ast::make_stmt(std::move(n));
    }),
            lexy::parse_state, lexy::values);
};

struct do_stmt
{
    static constexpr auto rule = dsl::position + kw_do + dsl::recurse<stmt> + dsl::position
        + dsl::must(kw_while).template error<e_while> + dsl::p<scan_until<cond_cfg>> + dsl::lit_c<')'> + semi;
    static constexpr auto value =
        lexy::bind(lexy::callback<StmtP>(
                       [](const ParseState& st, const char* pos, StmtP&& body, const char* wpos, Span cond) -> StmtP
    {
        ast::DoWhileStmt n;
        n.pos = st.offset(pos);
        n.while_pos = st.offset(wpos);
        n.body = std::move(body);
        n.cond = expr_of(st, cond);
        if (!n.cond || !n.body)
            return nullptr;
        return ast::make_stmt(std::move(n));
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- try
// "...": the clause catches all.
struct catch_all
{
    static constexpr auto rule = LEXY_LIT("...");
    static constexpr auto value = lexy::callback<ast::CatchClause>([]()
    {
        ast::CatchClause c;
        c.is_all = true;
        return c;
    });
};

// "Type name". A clause without a type or without a name has an empty name. The error is
// in the diagnostics. One word is the type, so "catch (int)" has no name.
struct catch_named
{
    static constexpr auto rule = dsl::position + dsl::p<scan_param_type> + dsl::opt(dsl::peek(id) >> dsl::p<name_p>);
    static constexpr auto value = lexy::bind(lexy::callback<ast::CatchClause>(
                                                 [](const ParseState& st, const char* pos, Span type, auto&& name)
    {
        ast::CatchClause c;
        constexpr bool no_name = std::is_same_v<std::decay_t<decltype(name)>, lexy::nullopt>;
        if (blank(st, type))
        {
            // The scan reads one word as the name. In "catch (int)" it is the type.
            st.error(pos, no_name ? "expected a type" : "expected a name");
        }
        else if constexpr (std::is_same_v<std::decay_t<decltype(name)>, lexy::nullopt>)
        {
            st.error(st.base + type.end, "expected a name");
        }
        else
        {
            c.name = std::move(name);
            c.type = type_of(st, type);
        }
        return c;
    }),
        lexy::parse_state, lexy::values);
};

struct catch_clause
{
    static constexpr auto rule = dsl::position + kw_catch + dsl::lit_c<'('>
        + (dsl::peek(LEXY_LIT("...")) >> dsl::p<catch_all> | dsl::else_ >> dsl::p<catch_named>)+dsl::lit_c<')'>
        + dsl::p<block_p>;
    static constexpr auto value =
        lexy::bind(lexy::callback<ast::CatchClause>(
                       [](const ParseState& st, const char* pos, ast::CatchClause&& c, StmtP&& body)
    {
        c.pos = st.offset(pos);
        c.block = std::move(std::get<ast::BlockStmt>(body->v));
        return std::move(c);
    }),
            lexy::parse_state, lexy::values);
};

struct catch_list
{
    static constexpr auto rule = dsl::list(dsl::peek(kw_catch) >> dsl::p<catch_clause>);
    static constexpr auto value = lexy::as_list<std::vector<ast::CatchClause>>;
};

// "finally { ... }". The value is the block.
struct finally_p
{
    static constexpr auto rule = kw_finally + dsl::p<block_p>;
    static constexpr auto value = lexy::forward<StmtP>;
};

// "try block catch..." with an optional "finally block", or "try block finally block".
// A try without a catch clause and without a finally block is an error.
struct try_stmt
{
    static constexpr auto rule = dsl::position + kw_try + dsl::p<block_p>
        + (dsl::peek(kw_catch) >> (dsl::p<catch_list> + dsl::opt(dsl::peek(kw_finally) >> dsl::p<finally_p>))
            | dsl::peek(kw_finally) >> dsl::p<finally_p> | dsl::error<e_catch>);

    static StmtP make(
        const ParseState& st, const char* pos, StmtP&& body, std::vector<ast::CatchClause>&& list, StmtP&& fin)
    {
        ast::TryStmt n;
        n.pos = st.offset(pos);
        n.block = std::move(std::get<ast::BlockStmt>(body->v));
        for (const ast::CatchClause& c : list)
            if (!c.is_all && c.name.empty())
                return nullptr;
        n.catches = std::move(list);
        if (fin)
            n.finally_block = std::move(std::get<ast::BlockStmt>(fin->v));
        return ast::make_stmt(std::move(n));
    }

    static constexpr auto value =
        lexy::bind(lexy::callback<StmtP>([](const ParseState& st, const char* pos, StmtP&& body,
                                             std::vector<ast::CatchClause>&& list, StmtP&& fin) -> StmtP
    { return make(st, pos, std::move(body), std::move(list), std::move(fin)); },
                       [](const ParseState& st, const char* pos, StmtP&& body, std::vector<ast::CatchClause>&& list,
                           lexy::nullopt) -> StmtP { return make(st, pos, std::move(body), std::move(list), nullptr); },
                       [](const ParseState& st, const char* pos, StmtP&& body, StmtP&& fin) -> StmtP
    { return make(st, pos, std::move(body), {}, std::move(fin)); }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- for
// "foreach ( (const)? (var &? | Type) name in expr ) stmt".
struct foreach_stmt
{
    static constexpr auto rule = dsl::position + kw_foreach + dsl::lit_c<'('> + dsl::p<var_head<DeclEnd::ForEach>>
        + kw_in + dsl::p<scan_until<part_cfg>> + dsl::lit_c<')'> + dsl::recurse<stmt>;
    static constexpr auto value =
        lexy::bind(lexy::callback<StmtP>(
                       [](const ParseState& st, const char* pos, VarHead&& h, Span range, StmtP&& body) -> StmtP
    {
        ast::ForOfStmt n;
        n.pos = st.offset(pos);
        n.is_const = h.is_const;
        n.is_ref = h.is_ref;
        n.name = std::move(h.name);
        n.names = std::move(h.names);
        n.name_poss = std::move(h.name_poss);
        n.name_pos = h.name_pos;
        n.type = std::move(h.type);
        if (!n.names.empty() && !binding_ok(st, n.pos, n.names.size()))
            return nullptr;
        n.range = expr_of(st, range);
        n.body = std::move(body);
        if (!n.range || !n.body)
            return nullptr;
        return ast::make_stmt(std::move(n));
    }),
            lexy::parse_state, lexy::values);
};

struct for_init
{
    static constexpr auto rule = dsl::peek(var_or_const) >> dsl::p<decl_p<for_init_cfg>>
        | decl_ahead >> dsl::p<decl_p<for_init_cfg>> | dsl::else_ >> dsl::p<scan_until<part_cfg>>;
    static constexpr auto value = lexy::callback<ForInit>(
        [](DeclParts&& d)
    {
        ForInit i;
        i.is_decl = true;
        i.decl = std::move(d);
        return i;
    }, [](Span sp)
    {
        ForInit i;
        i.span = sp;
        return i;
    });
};

struct for_classic_rest
{
    static constexpr auto rule = dsl::lit_c<'('> + dsl::p<for_init> + dsl::lit_c<';'> + dsl::p<scan_until<part_cfg>>
        + dsl::lit_c<';'> + dsl::p<scan_until<part_cfg>> + dsl::lit_c<')'> + dsl::recurse<stmt>;
    static constexpr auto value = lexy::callback<ForRest>([](ForInit&& i, Span cond, Span step, StmtP&& body)
    { return ForRest{std::move(i), cond, step, std::move(body)}; });
};

struct for_stmt
{
    static constexpr auto rule = dsl::position + kw_for + dsl::p<for_classic_rest>;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>(
                                                 [](const ParseState& st, const char* pos, ForRest&& r) -> StmtP
    {
        ast::ForStmt n;
        n.pos = st.offset(pos);
        bool ok = true;
        if (r.init.is_decl)
        {
            StmtP d = make_decl(st, std::move(r.init.decl));
            if (d)
                n.init.push_back(std::move(d));
            else
                ok = false;
        }
        else
        {
            std::vector<ExprP> list;
            if (list_of(st, r.init.span, list))
            {
                for (ExprP& e : list)
                {
                    const uint32_t p = ast::pos_of(*e);
                    n.init.push_back(ast::make_stmt(ast::ExprStmt{std::move(e), p}));
                }
            }
            else
            {
                ok = false;
            }
        }
        if (!blank(st, r.cond))
        {
            n.cond = expr_of(st, r.cond);
            if (!n.cond)
                ok = false;
        }
        if (!list_of(st, r.step, n.step))
            ok = false;
        n.body = std::move(r.body);
        if (!ok || !n.body)
            return nullptr;
        return ast::make_stmt(std::move(n));
    }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- switch
// The text of a case label. It ends before the first ":" at depth 0.
// A ";" and a "}" at depth 0 end it too.
struct case_span : lexy::token_production
{
    static constexpr auto rule = dsl::position
        + dsl::loop(dsl::peek(dsl::eof | (dsl::lit_c<':'> / dsl::lit_c<';'> / dsl::lit_c<'}'>)) >> dsl::break_
            | dsl::else_ >> dsl::p<until_item>)
        + dsl::position;
    static constexpr auto value = span_value;
};

struct case_label
{
    static constexpr auto rule = dsl::position + kw_case + dsl::p<case_span> + dsl::lit_c<':'>;
    static constexpr auto value = lexy::bind(lexy::callback<SwItem>(
                                                 [](const ParseState& st, const char* pos, Span label)
    {
        SwItem i;
        i.kind = SwItem::Case;
        i.pos = st.offset(pos);
        if (blank(st, label))
            st.error(st.base + label.begin, "expected an expression");
        else
            i.label = expr_of(st, label);
        return i;
    }),
        lexy::parse_state, lexy::values);
};

struct default_label
{
    static constexpr auto rule = dsl::position + kw_default + dsl::lit_c<':'>;
    static constexpr auto value = lexy::bind(lexy::callback<SwItem>(
                                                 [](const ParseState& st, const char* pos)
    {
        SwItem i;
        i.kind = SwItem::Default;
        i.pos = st.offset(pos);
        return i;
    }),
        lexy::parse_state, lexy::values);
};

struct sw_item
{
    static constexpr auto rule = dsl::peek(kw_case) >> dsl::p<case_label>
        | dsl::peek(kw_default) >> dsl::p<default_label> | dsl::else_ >> dsl::recurse<stmt>;
    static constexpr auto value =
        lexy::bind(lexy::callback<SwItem>([](const ParseState&, SwItem&& i) { return std::move(i); },
                       [](const ParseState&, StmtP&& s)
    {
        SwItem i;
        i.kind = SwItem::Statement;
        if (s)
            i.pos = ast::pos_of(*s);
        i.stmt = std::move(s);
        return i;
    }),
            lexy::parse_state, lexy::values);
};

struct sw_items
{
    static constexpr auto item = dsl::peek_not(inner_cfg::closer)
        >> dsl::try_(dsl::p<sw_item>, dsl::p<sync<inner_cfg, SwItem>>);
    static constexpr auto rule = dsl::opt(dsl::peek_not(inner_cfg::closer) >> dsl::list(item));
    static constexpr auto value = lexy::as_list<std::vector<SwItem>>;
};

struct switch_stmt
{
    static constexpr auto rule = dsl::position + kw_switch + dsl::p<scan_until<cond_cfg>> + dsl::lit_c<')'>
        + dsl::lit_c<'{'> + dsl::p<sw_items> + dsl::position + dsl::lit_c<'}'>;
    static constexpr auto value = lexy::bind(lexy::callback<StmtP>(
                                                 [](const ParseState& st, const char* pos, Span subject,
                                                     std::vector<SwItem>&& items, const char* end) -> StmtP
    {
        ast::SwitchStmt n;
        n.pos = st.offset(pos);
        n.end_pos = st.offset(end);
        n.subject = expr_of(st, subject);
        // A label after a statement, or a case after a default, starts a new group.
        bool open = false;
        for (SwItem& it : items)
        {
            if (it.kind == SwItem::None)
                continue;
            if (it.kind == SwItem::Statement)
            {
                if (!it.stmt)
                    continue;
                if (!open)
                {
                    st.error(st.base + it.pos, "expected 'case' or 'default'");
                    continue;
                }
                n.cases.back().stmts.push_back(std::move(it.stmt));
                continue;
            }
            const bool fresh = n.cases.empty() || !open || !n.cases.back().stmts.empty() || n.cases.back().is_default;
            if (fresh)
            {
                n.cases.emplace_back();
                n.cases.back().pos = it.pos;
            }
            open = true;
            if (it.kind == SwItem::Default)
            {
                n.cases.back().is_default = true;
                n.cases.back().default_pos = it.pos;
            }
            else if (it.label)
            {
                n.cases.back().labels.push_back(std::move(it.label));
                n.cases.back().label_pos.push_back(it.pos);
            }
        }
        if (!n.subject)
            return nullptr;
        return ast::make_stmt(std::move(n));
    }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- statement
struct stmt
{
    static constexpr auto rule = dsl::peek(dsl::lit_c<'{'>) >> dsl::p<block_p>
        | dsl::peek(var_or_const) >> dsl::p<decl_stmt> | dsl::peek(kw_if) >> dsl::p<if_stmt>
        | dsl::peek(kw_while) >> dsl::p<while_stmt> | dsl::peek(kw_do) >> dsl::p<do_stmt>
        | dsl::peek(kw_for) >> dsl::p<for_stmt> | dsl::peek(kw_foreach) >> dsl::p<foreach_stmt>
        | dsl::peek(kw_return) >> dsl::p<return_stmt> | dsl::peek(kw_break) >> dsl::p<break_stmt>
        | dsl::peek(kw_continue) >> dsl::p<continue_stmt> | dsl::peek(kw_switch) >> dsl::p<switch_stmt>
        | dsl::peek(kw_throw) >> dsl::p<throw_stmt> | dsl::peek(kw_try) >> dsl::p<try_stmt>
        | dsl::peek(dsl::lit_c<';'>) >> dsl::p<empty_stmt> | dsl::peek(dsl::eof | dsl::lit_c<'}'>) >> dsl::error<e_stmt>
        | dsl::peek(dsl::lit_c<'['>) >> dsl::p<attr_decl_stmt> | decl_ahead >> dsl::p<decl_stmt>
        | dsl::else_ >> dsl::p<expr_stmt>;
    static constexpr auto value = lexy::forward<StmtP>;
};

// ---------------------------------------------------------------- the entry point
struct block_entry
{
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_recursion_depth = max_depth;
    static constexpr auto rule = dsl::whitespace(ws_rule) + dsl::p<stmts_p<top_cfg>>;
    static constexpr auto value = lexy::forward<std::vector<StmtP>>;
};

// Counts the nested calls of parse_block in this thread.
struct NestingGuard
{
    static thread_local int depth;
    NestingGuard() { ++depth; }
    ~NestingGuard() { --depth; }
    NestingGuard(const NestingGuard&) = delete;
    NestingGuard& operator=(const NestingGuard&) = delete;
};
thread_local int NestingGuard::depth = 0;

} // namespace

ast::BlockStmt parse_block(ParseInput in, Span body_without_braces, Diagnostics& diag)
{
    ast::BlockStmt block;
    block.pos = body_without_braces.begin > 0 ? body_without_braces.begin - 1 : 0;
    block.end_pos = body_without_braces.end;

    const std::size_t size = in.text.size();
    const std::size_t end = body_without_braces.end < size ? body_without_braces.end : size;
    const std::size_t begin = body_without_braces.begin < end ? body_without_braces.begin : end;

    NestingGuard guard;
    if (NestingGuard::depth > max_block_nesting)
    {
        diag.add(static_cast<uint32_t>(begin), *in.map, "maximum nesting depth exceeded");
        return block;
    }
    const size_t first_item = diag.items.size();
    const ParseState st{in.text.data(), in.map, &diag, size, in.array_tpl};
    const auto input = lexy::string_input<Encoding>(in.text.data() + begin, end - begin);
    auto result = parse_with_diag<block_entry>(input, st);
    // The parse reports the recursion limit again at each column. Keep the first error and
    // drop the errors after it: the rest of the body is not parsed.
    for (size_t i = first_item; i < diag.items.size(); ++i)
    {
        if (diag.items[i].msg == "maximum recursion depth exceeded")
        {
            diag.items.resize(i + 1);
            break;
        }
    }
    if (result.has_value())
        block.stmts = keep(std::move(result).value());
    return block;
}

} // namespace hotc
