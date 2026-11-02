// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The grammar of the expressions.
//
// The grammar is one lexy expression_production. It builds the Expr nodes of ast.h.
// A parent grammar scans the span of an expression and calls parse_expression. The
// function parses the span in the same buffer, so each position is an absolute offset.
//
// Two rules need a look-ahead and not a recursion, because dsl::peek cannot recurse:
//  - A "(" starts a lambda when a scan to the matching ")" finds a "=>" after it. The scan
//    has a fixed nesting depth (lambda_depth). A parameter is "Type name" or "name".
//    The old form "(" name ":" is also a lambda path, so it gives the error
//    "parameter syntax changed; write 'Type name'".
//  - A "<" starts template arguments when a scan of "<...>" ends before a "(" or a
//    ".". The scan has a fixed nesting depth (targs_depth). It stops at a newline,
//    at ";" and at each char that a type cannot have.
#include "expression.h"
#include "lexy_common.hpp"
#include "statement.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace hotc
{
namespace
{

using namespace parser; // NOLINT: the grammar uses the names of lexy_common.hpp
using ast::ExprP;

// ---------------------------------------------------------------- error texts
// The script has no "::" and no "->". A "." does both jobs. The errors tell the user.
HOT_ERROR_TAG(e_expected_expr, "expected an expression");
HOT_ERROR_TAG(e_end_of_expr, "expected end of expression");
HOT_ERROR_TAG(e_test_array, "is and as do not accept an array type");
HOT_ERROR_TAG(e_brace_init, "brace initialization is not supported; use parentheses");
HOT_ERROR_TAG(e_new_paren, "expected '(' after the type of 'new'");
HOT_ERROR_TAG(e_targs, "not a template argument list");

// ---------------------------------------------------------------- limits
// The nesting of the brackets in the text of template arguments.
constexpr int targs_depth = 4;
// The nesting of "(" in the parameters of a lambda, for example "std.function<void(int)> f".
constexpr int lambda_depth = 4;
// The limit of dsl::recurse: nested "(", "[", "{", "?" and argument lists. Each level
// uses some stack, so the limit is low. A script does not nest deeper.
constexpr std::size_t max_depth = 256;

// ---------------------------------------------------------------- words
constexpr auto kw_const = LEXY_KEYWORD("const", id);
constexpr auto kw_this = LEXY_KEYWORD("this", id);
constexpr auto kw_base = LEXY_KEYWORD("base", id);
constexpr auto kw_null = LEXY_KEYWORD("null", id);
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

// The words of the body syntax. A member name after a "." can be any word of this set.
// The set has no "get", "set" and "in", because a native method can have such a name.
constexpr auto id_m = id.reserve(kw_const, kw_this, kw_base, kw_null, kw_if, kw_else, kw_while, kw_do, kw_for,
    kw_return, kw_break, kw_continue, kw_switch, kw_case, kw_default, kw_try, kw_catch, kw_throw);

// A name. The words of the body syntax and the reserved words are not names.
constexpr auto id_r =
    id.reserve(kw_const, kw_this, kw_base, kw_null, kw_if, kw_else, kw_while, kw_do, kw_for, kw_return, kw_break,
        kw_continue, kw_switch, kw_case, kw_default, kw_try, kw_catch, kw_throw, kw_var, kw_foreach, kw_in, kw_using,
        kw_override, kw_virtual, kw_async, kw_await, kw_new, kw_public, kw_private, kw_protected, kw_get, kw_set,
        kw_finally, kw_void);

// ---------------------------------------------------------------- plain helpers
std::string_view span_text(const ParseState& st, Span sp)
{
    return std::string_view(st.base + sp.begin, sp.end - sp.begin);
}

// The type text of a span. A "." is a scope and becomes "::". A "::" is an error.
std::string type_of(const ParseState& st, Span sp)
{
    return type_of_span(st, sp);
}

// Splits the raw text of a qualified name. The text is part ("." part)*. A part can have
// template arguments. The arguments of the last part go to template_args. The arguments of
// the other parts stay in the text of the part, for example "A<int>". In both texts a "."
// between two names becomes "::" (see scope_text). A "::" in the arguments is an error.
// The arguments of the last part are also parsed as a list of expressions, when that works.
// The try is quiet: a failed try records no error. The text stays in template_args.
ast::NameExpr split_name(const ParseState& st, std::string_view t, uint32_t pos)
{
    ast::NameExpr n;
    n.pos = pos;
    std::size_t i = 0;
    while (i < t.size())
    {
        const std::size_t b = i;
        while (i < t.size() && is_id(t[i]))
            ++i;
        std::string part(t.substr(b, i - b));
        n.last_pos = pos + static_cast<uint32_t>(b); // the raw offset of this part
        n.part_pos.push_back(n.last_pos);
        if (i < t.size() && t[i] == '<')
        {
            int depth = 0;
            std::size_t j = i;
            for (; j < t.size(); ++j)
            {
                if (t[j] == '<')
                    ++depth;
                else if (t[j] == '>' && --depth == 0)
                    break;
            }
            const std::size_t e = j < t.size() ? j + 1 : t.size();
            const std::string targs = array_types(st, st.base + pos + i,
                canonical_type_at(st, st.base + pos + i, scope_text(st, st.base + pos + i, t.substr(i, e - i))), false);
            const std::size_t targs_at = i;
            i = e;
            if (i >= t.size())
            {
                n.template_args = targs;
                if (targs.size() > 2)
                {
                    // The offsets of the raw text. The scope text can be longer than the raw text.
                    const uint32_t targs_begin = pos + static_cast<uint32_t>(targs_at) + 1;
                    const Span args{targs_begin, pos + static_cast<uint32_t>(e) - 1};
                    Diagnostics quiet;
                    std::vector<ExprP> list =
                        parse_expression_list(ParseInput{std::string_view(st.base, st.size), st.map, st.array_tpl, st.aliases}, args, quiet);
                    if (quiet.items.empty() && !list.empty())
                    {
                        n.targ_exprs = std::move(list);
                        n.targ_parsed = true;
                    }
                }
            }
            else
                part += targs;
        }
        n.parts.push_back(std::move(part));
        if (i >= t.size() || t[i] != '.')
            break;
        ++i;
    }
    // "Alias.Member": the first part is a type alias of a plain name ("using C = ns.Color;").
    // The parts of the target replace it. An alias of a template instance has no such form.
    if (st.aliases && n.parts.size() >= 2)
    {
        const auto it = st.aliases->find(n.parts[0]);
        if (it != st.aliases->end() && it->second.find_first_of("<>(),*& ") == std::string::npos)
        {
            std::vector<std::string> head;
            std::string target = it->second;
            for (std::size_t b = 0; b <= target.size();)
            {
                const std::size_t e = std::min(target.find("::", b), target.size());
                std::string seg = target.substr(b, e - b);
                if (!seg.empty() && seg.front() == '\x01')
                    seg.erase(0, 1);
                head.push_back(std::move(seg));
                b = e + 2;
            }
            const std::size_t extra = head.size() - 1;
            n.parts.erase(n.parts.begin());
            n.parts.insert(n.parts.begin(), head.begin(), head.end());
            // The offsets of the new parts are the offset of the alias name.
            if (!n.part_pos.empty())
                n.part_pos.insert(n.part_pos.begin(), extra, n.part_pos[0]);
        }
    }
    return n;
}

// ---------------------------------------------------------------- operator tags
// An operator name as template arguments: up to 3 chars. The macro HOT_CH3 splits a literal.
// A class type as a template argument needs C++20, so the name is a list of chars.
constexpr char op_char(const char* s, std::size_t i)
{
    return s[0] == 0 ? char(0) : (i == 0 ? s[0] : op_char(s + 1, i - 1));
}
#define HOT_CH3(S) op_char(S, 0), op_char(S, 1), op_char(S, 2)
template <char C0, char C1, char C2> struct OpName
{
    static constexpr char v[4] = {C0, C1, C2, 0};
};

template <char C0, char C1, char C2> struct Bin
{
}; // an infix operator, also an assignment
template <char C0, char C1, char C2> struct Post
{
}; // "++" or "--" after the operand
template <char C0, char C1, char C2> struct Member
{
}; // "."
// The name after a ".": the text and the byte offset. The text is empty when the name is missing.
struct SafeTag
{
}; // "?."
struct MemberName
{
    std::string name;
    uint32_t pos = 0;
};
template <char C0, char C1, char C2> struct Pre
{ // a prefix operator; it keeps the position of the operator
    const char* pos;
    explicit Pre(const char* p)
        : pos(p)
    {
    }
};
// The prefix operator "await". Its name has more than 3 chars, so it has its own tag.
struct AwaitTag
{
    const char* pos;
    explicit AwaitTag(const char* p)
        : pos(p)
    {
    }
};
struct CondTag
{
};
struct CallTag
{
};
struct IsTag
{
}; // "obj is T"
struct AsTag
{
}; // "obj as T"
struct ArrowTag
{
}; // "->": always an error
struct IndexTag
{
};

#define HOT_BIN(S) dsl::op<Bin<HOT_CH3(S)>>(LEXY_LIT(S))
#define HOT_BIN_NF(S, CC) dsl::op<Bin<HOT_CH3(S)>>(dsl::not_followed_by(LEXY_LIT(S), CC))
#define HOT_PRE(S) dsl::op<Pre<HOT_CH3(S)>>(LEXY_LIT(S))
#define HOT_PRE_NF(S, CC) dsl::op<Pre<HOT_CH3(S)>>(dsl::not_followed_by(LEXY_LIT(S), CC))
#define HOT_POST(S) dsl::op<Post<HOT_CH3(S)>>(LEXY_LIT(S))

// The chars that an operator must not have after it.
constexpr auto cc_eq = dsl::lit_c<'='>;
constexpr auto cc_plus = dsl::lit_c<'+'> / dsl::lit_c<'='>;
constexpr auto cc_minus = dsl::lit_c<'-'> / dsl::lit_c<'='> / dsl::lit_c<'>'>;
constexpr auto cc_lt = dsl::lit_c<'<'> / dsl::lit_c<'='>;
constexpr auto cc_gt = dsl::lit_c<'>'> / dsl::lit_c<'='>;
constexpr auto cc_amp = dsl::lit_c<'&'> / dsl::lit_c<'='>;
constexpr auto cc_bar = dsl::lit_c<'|'> / dsl::lit_c<'='>;
constexpr auto cc_plus_only = dsl::lit_c<'+'>;
constexpr auto cc_minus_only = dsl::lit_c<'-'>;
constexpr auto cc_amp_only = dsl::lit_c<'&'>;

struct expr;
struct unary_expr; // the operand of a cast: a prefix operator or an atom with postfix operators

// ---------------------------------------------------------------- the nodes
// The value of an expression production. Each atom production gives an ExprP.
// The first argument is the parse state. Only the prefix operators use it.
struct Builder
{
    ExprP operator()(const ParseState&, ExprP e) const { return e; }

    template <char C0, char C1, char C2> ExprP operator()(const ParseState&, ExprP l, Bin<C0, C1, C2>, ExprP r) const
    {
        const uint32_t pos = ast::pos_of(*l);
        return ast::make_expr(ast::BinaryExpr{OpName<C0, C1, C2>::v, std::move(l), std::move(r), pos});
    }
    template <char C0, char C1, char C2> ExprP operator()(const ParseState& st, Pre<C0, C1, C2> t, ExprP e) const
    {
        return ast::make_expr(ast::UnaryExpr{OpName<C0, C1, C2>::v, std::move(e), false, st.offset(t.pos)});
    }
    ExprP operator()(const ParseState& st, AwaitTag t, ExprP e) const
    {
        return ast::make_expr(ast::UnaryExpr{"await", std::move(e), false, st.offset(t.pos)});
    }
    template <char C0, char C1, char C2> ExprP operator()(const ParseState&, ExprP e, Post<C0, C1, C2>) const
    {
        const uint32_t pos = ast::pos_of(*e);
        return ast::make_expr(ast::UnaryExpr{OpName<C0, C1, C2>::v, std::move(e), true, pos});
    }
    ExprP operator()(const ParseState&, ExprP c, CondTag, ExprP a, ExprP b) const
    {
        const uint32_t pos = ast::pos_of(*c);
        return ast::make_expr(ast::TernaryExpr{std::move(c), std::move(a), std::move(b), pos});
    }
    ExprP operator()(const ParseState&, ExprP f, CallTag, std::vector<ExprP> args) const
    {
        const uint32_t pos = ast::pos_of(*f);
        return ast::make_expr(ast::CallExpr{std::move(f), std::move(args), pos});
    }
    ExprP operator()(const ParseState&, ExprP o, IndexTag, ExprP i) const
    {
        const uint32_t pos = ast::pos_of(*o);
        return ast::make_expr(ast::IndexExpr{std::move(o), std::move(i), pos});
    }
    // "obj is T" and "obj as T" (design 7.7). The type text is C++ text, as in "new T(...)".
    ExprP operator()(const ParseState& st, ExprP o, IsTag, Span name, lexy::nullopt) const
    {
        return test(st, std::move(o), name, false);
    }
    // "obj is T name": the test with a binding.
    ExprP operator()(const ParseState& st, ExprP o, IsTag, Span name, Span bind) const
    {
        ExprP e = test(st, std::move(o), name, false);
        auto& t = std::get<ast::TypeTestExpr>(e->v);
        t.bind = std::string(span_text(st, bind));
        t.bind_pos = bind.begin;
        return e;
    }
    ExprP operator()(const ParseState& st, ExprP o, AsTag, Span name) const
    {
        return test(st, std::move(o), name, true);
    }
    static ExprP test(const ParseState& st, ExprP o, Span name, bool is_as)
    {
        const uint32_t pos = ast::pos_of(*o);
        std::string type =
            canonical_type_at(st, st.base + name.begin, scope_text(st, st.base + name.begin, span_text(st, name)));
        return ast::make_expr(ast::TypeTestExpr{std::move(o), std::move(type), is_as, pos});
    }
    // "a->b": the parse reported the error and read the name. The result is not used.
    ExprP operator()(const ParseState&, ExprP o, ArrowTag, std::string) const { return o; }
    // "o?.name". The lowering allows it only at the start of an expression statement.
    ExprP operator()(const ParseState&, ExprP o, SafeTag, MemberName&& name) const
    {
        const uint32_t pos = ast::pos_of(*o);
        ast::MemberExpr m{std::move(o), std::move(name.name), pos, name.pos};
        m.safe = true;
        return ast::make_expr(std::move(m));
    }
    template <char C0, char C1, char C2> ExprP operator()(const ParseState&, ExprP o, Member<C0, C1, C2>, MemberName&& name) const
    {
        const uint32_t pos = ast::pos_of(*o);
        return ast::make_expr(ast::MemberExpr{std::move(o), std::move(name.name), pos, name.pos});
    }
};

// ---------------------------------------------------------------- tokens
struct id_str : lexy::token_production
{
    static constexpr auto rule = id;
    static constexpr auto value = lexy::as_string<std::string>;
};

// The name after a ".". When there is no name, the parse adds an error and the name is empty.
// The tree keeps the node, so a language server can complete the name (tolerant parse).
struct member_name_p
{
    static constexpr auto rule = dsl::peek(id) >> (dsl::position + dsl::p<id_str>) | dsl::else_ >> dsl::position;
    static constexpr auto value =
        lexy::bind(lexy::callback<MemberName>([](const ParseState& st, const char* pos, std::string&& name)
    { return MemberName{std::move(name), st.offset(pos)}; },
                       [](const ParseState& st, const char* pos)
    {
        // The hole is right after the ".", before any white space.
        uint32_t dot_end = st.offset(pos);
        while (dot_end > 0 && std::isspace(static_cast<unsigned char>(st.base[dot_end - 1])))
            --dot_end;
        st.diag->add(dot_end, *st.map, "expected a member name after '.'");
        st.diag->member_holes.push_back(dot_end);
        return MemberName{std::string(), st.offset(pos)};
    }),
            lexy::parse_state, lexy::values);
};

// A number. The rules are the rules of the old lexer. The text stays raw.
// A sign belongs to the number after an "e" or an "E", but not in a hex number.
constexpr auto num_word = dsl::ascii::word / dsl::lit_c<'.'>;
constexpr auto num_exp = (dsl::lit_c<'e'> / dsl::lit_c<'E'>) >> dsl::if_(dsl::lit_c<'+'> / dsl::lit_c<'-'>);
constexpr auto num_hex = dsl::literal_set(LEXY_LIT("0x"), LEXY_LIT("0X")) >> dsl::while_(num_word);

struct number_p : lexy::token_production
{
    static constexpr auto rule =
        dsl::position + (num_hex | dsl::else_ >> dsl::while_(num_exp | num_word)) + dsl::position;
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>([](const ParseState& st, const char* b, const char* e)
    { return ast::make_expr(ast::NumberExpr{std::string(b, e), st.offset(b)}); }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- string interpolation
// The scanners below read the text of a string that lexy has already accepted. "p" is after
// the "${". They return the position of the matching "}".
const char* scan_interp(const char* p, const char* end);

// "p" is after the opening quote. Returns the position after the closing quote.
const char* skip_quoted(const char* p, const char* end, char quote)
{
    while (p < end)
    {
        if (*p == '\\')
        {
            p += 2;
        }
        else if (*p == quote)
        {
            return p + 1;
        }
        else if (quote == '"' && *p == '$' && p + 1 < end && p[1] == '{')
        {
            const char* c = scan_interp(p + 2, end);
            if (!c)
                return end;
            p = c + 1;
        }
        else
        {
            ++p;
        }
    }
    return end;
}

const char* scan_interp(const char* p, const char* end)
{
    int depth = 0;
    while (p < end)
    {
        const char c = *p;
        if (c == '"' || c == '\'')
            p = skip_quoted(p + 1, end, c);
        else if (c == '{')
        {
            ++depth;
            ++p;
        }
        else if (c == '}')
        {
            if (depth == 0)
                return p;
            --depth;
            ++p;
        }
        else
            ++p;
    }
    return nullptr;
}

// The place of the ":" that starts the format spec in the text of "${...}", or null. The colon
// must be at depth 0, not a part of "::" and not the colon of "a ? b : c".
const char* find_spec(const char* b, const char* e)
{
    int depth = 0, pending = 0;
    for (const char* p = b; p < e;)
    {
        const char c = *p;
        if (c == '"' || c == '\'')
        {
            p = skip_quoted(p + 1, e, c);
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (depth == 0 && c == '?')
            ++pending;
        else if (depth == 0 && c == ':')
        {
            if (p + 1 < e && p[1] == ':')
            {
                p += 2;
                continue;
            }
            if (pending > 0)
                --pending;
            else
                return p;
        }
        ++p;
    }
    return nullptr;
}

// "text" is a string literal with its quotes. Gives a StringExpr, or an InterpExpr when the
// string has "${". A string with no "${" is the same node as before.
ExprP make_string(const ParseState& st, const char* b, const char* e)
{
    auto plain = [&] { return ast::make_expr(ast::StringExpr{std::string(b, e), st.offset(b)}); };
    bool found = false;
    for (const char* p = b + 1; p + 1 < e; ++p)
    {
        if (*p == '\\')
            ++p;
        else if (*p == '$' && p[1] == '{')
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        // A "\${" escape alone also needs the interpolation node: the escape is not C++.
        if (std::string_view(b, e - b).find("\\${") == std::string_view::npos)
            return plain();
    }
    ast::InterpExpr n;
    n.pos = st.offset(b);
    std::string cur;
    const char* close_quote = e - 1;
    const ParseInput in{std::string_view(st.base, st.size), st.map, st.array_tpl, st.aliases};
    for (const char* p = b + 1; p < close_quote;)
    {
        if (*p == '\\' && p + 1 < close_quote)
        {
            cur.append(p, 2);
            p += 2;
        }
        else if (*p == '$' && p + 1 < close_quote && p[1] == '{')
        {
            const char* c = scan_interp(p + 2, close_quote);
            if (!c)
                return plain();
            const char* spec = find_spec(p + 2, c);
            const char* ex_end = spec ? spec : c;
            ast::ExprP arg = parse_expression(in, Span{st.offset(p + 2), st.offset(ex_end)}, *st.diag);
            if (!arg)
                return plain(); // the parser added the error
            n.lits.push_back(std::move(cur));
            cur.clear();
            n.args.push_back(std::move(arg));
            n.specs.push_back(spec ? std::string(spec + 1, c) : std::string());
            p = c + 1;
        }
        else
        {
            cur.push_back(*p++);
        }
    }
    n.lits.push_back(std::move(cur));
    return ast::make_expr(std::move(n));
}

// A string or a character literal. The text keeps the quotes.
template <typename Node, char Quote> struct literal_p : lexy::token_production
{
    static constexpr auto rule = dsl::position + quoted<Quote> + dsl::position;
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>(
                       [](const ParseState& st, const char* b, const char*, const char*, const char* e)
    {
        if constexpr (std::is_same_v<Node, ast::StringExpr>)
            return make_string(st, b, e);
        else
            return ast::make_expr(Node{std::string(b, e), st.offset(b)});
    }),
            lexy::parse_state, lexy::values);
};

// this, base, null
template <typename Node, typename Kw> struct kw_p : lexy::token_production
{
    static constexpr auto rule = dsl::position + dsl::token(Kw{});
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>([](const ParseState& st, const char* b) { return ast::make_expr(Node{st.offset(b)}); }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- template arguments
// The text after "<" up to the matching ">". It has a fixed nesting depth. A char that
// a type cannot have fails the scan, so "a < b && c > (d)" is not a template call.
template <int N> struct targs_body : lexy::token_production
{
    static constexpr auto rule = []
    {
        auto plain = dsl::ascii::alnum / dsl::lit_c<'_'> / dsl::lit_c<' '> / dsl::lit_c<'\t'> / dsl::lit_c<','>
            / dsl::lit_c<'*'> / dsl::lit_c<'.'> / dsl::lit_c<'&'>;
        auto head = dsl::lit_c<'>'> >> dsl::break_ | LEXY_LIT("&&") >> dsl::error<e_targs> | LEXY_LIT("::") | plain;
        if constexpr (N > 0)
            return dsl::loop(head | dsl::lit_c<'<'> >> dsl::p<targs_body<N - 1>> | dsl::else_ >> dsl::error<e_targs>);
        else
            return dsl::loop(head | dsl::else_ >> dsl::error<e_targs>);
    }();
    static constexpr auto value = lexy::noop;
};

struct targs_tok : lexy::token_production
{
    static constexpr auto rule = dsl::lit_c<'<'> + dsl::p<targs_body<targs_depth>>;
    static constexpr auto value = lexy::noop;
};

// "<...>" that is followed by "(" or ".".
constexpr auto targs_head = dsl::lit_c<'<'> + dsl::p<targs_body<targs_depth>>
    + dsl::while_(dsl::lit_c<' '> / dsl::lit_c<'\t'>) + dsl::literal_set(dsl::lit_c<'('>, dsl::lit_c<'.'>);

// ---------------------------------------------------------------- names
// The value is the span of the whole name, with the template arguments.
// The parts are joined by ".". A "::" is an error.
struct qname_tok : lexy::token_production
{
    static constexpr auto rule = []
    {
        auto first = dsl::token(id_r) + dsl::if_(dsl::peek(targs_head) >> dsl::p<targs_tok>);
        auto seg = dsl::token(id_m) + dsl::if_(dsl::peek(targs_head) >> dsl::p<targs_tok>);
        return dsl::position + first
            + dsl::loop(dsl::peek(dsl::lit_c<'.'> + id_m) >> (dsl::lit_c<'.'> + seg)
                | dsl::peek(LEXY_LIT("::")) >> dsl::error<e_use_dot_colons> | dsl::else_ >> dsl::break_)
            + dsl::position;
    }();
    static constexpr auto value = span_value;
};

constexpr auto kw_is = LEXY_KEYWORD("is", id);
constexpr auto kw_as = LEXY_KEYWORD("as", id);

// The name of a binding: "x is T name".
constexpr auto id_b = id_r.reserve(kw_is, kw_as);
struct bind_tok : lexy::token_production
{
    static constexpr auto rule = dsl::position + dsl::token(id_b) + dsl::position;
    static constexpr auto value = span_value;
};

// The elements of a list up to the closer. The opener is already read.
template <char Close> struct elems_p
{
    static constexpr auto rule =
        dsl::opt(dsl::peek_not(dsl::lit_c<Close>) >> dsl::list(dsl::recurse<expr>, dsl::sep(dsl::comma)))
        + dsl::lit_c<Close>;
    static constexpr auto value = lexy::as_list<std::vector<ExprP>>;
};

// A name. A "{" after the name is an error: "Vec3{1, 2, 3}" is not supported.
struct name_p
{
    static constexpr auto rule = dsl::p<qname_tok> + dsl::if_(dsl::peek(dsl::lit_c<'{'>) >> dsl::error<e_brace_init>);
    static constexpr auto value = lexy::bind(lexy::callback<ExprP>([](const ParseState& st, Span name)
    { return ast::make_expr(split_name(st, span_text(st, name), name.begin)); }),
        lexy::parse_state, lexy::values);
};

// new T(args). The type is a qualified name with optional template arguments.
struct new_p
{
    static constexpr auto rule = dsl::position + dsl::token(kw_new) + dsl::p<qname_tok>
        + (dsl::peek(dsl::lit_c<'{'>) >> dsl::error<e_new_paren>
            | dsl::else_ >> (dsl::lit_c<'('> + dsl::p<elems_p<')'>>));
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>(
                       [](const ParseState& st, const char* pos, Span name, std::vector<ExprP>&& args)
    {
        return ast::make_expr(ast::NewExpr{
            canonical_type_at(st, st.base + name.begin, scope_text(st, st.base + name.begin, span_text(st, name))),
            std::move(args), st.offset(pos)});
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- collection literal
// "[a, b]" or "[k: v, ...]". An entry is an expression, with an optional ": value" after it.
// A ternary key needs no parentheses: its own ":" is read first.
struct CollEntry
{
    ExprP key, value; // value is null for a list element
};

struct coll_entry
{
    static constexpr auto rule = dsl::recurse<expr> + dsl::opt(dsl::lit_c<':'> >> dsl::recurse<expr>);
    static constexpr auto value = lexy::callback<CollEntry>([](ExprP&& k, lexy::nullopt) {
        return CollEntry{std::move(k), nullptr};
    }, [](ExprP&& k, ExprP&& v) { return CollEntry{std::move(k), std::move(v)}; });
};

struct coll_entries
{
    static constexpr auto rule =
        dsl::opt(dsl::peek_not(dsl::lit_c<']'>) >> dsl::list(dsl::p<coll_entry>, dsl::sep(dsl::comma)));
    static constexpr auto value = lexy::as_list<std::vector<CollEntry>>;
};

struct coll_p
{
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'['> + dsl::p<coll_entries> + dsl::position + dsl::lit_c<']'>;
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>(
                       [](const ParseState& st, const char* pos, std::vector<CollEntry>&& es, const char* end)
    {
        ast::CollectionExpr c;
        c.pos = st.offset(pos);
        c.end_pos = st.offset(end);
        c.is_map = !es.empty() && es[0].value;
        for (CollEntry& en : es)
        {
            if (c.is_map && !en.value)
            {
                st.error(st.base + ast::pos_of(*en.key), "a map entry needs a value: key: value");
                continue;
            }
            if (!c.is_map && en.value)
            {
                st.error(st.base + ast::pos_of(*en.key), "a list element has no value");
                continue;
            }
            c.elems.push_back(std::move(en.key));
            if (c.is_map)
                c.values.push_back(std::move(en.value));
        }
        return ast::make_expr(std::move(c));
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- lambda
// An attribute such as "[weak]" before a lambda parameter. It is an error: the rule reads it and
// reports it, so the message is clear.
struct lambda_attr_p
{
    static constexpr auto rule = dsl::lit_c<'['> + dsl::position + dsl::p<id_str> + dsl::lit_c<']'>;
    static constexpr auto value = lexy::bind(lexy::callback<int>([](const ParseState& st, const char* pos, std::string name)
    {
        if (name == "weak")
            st.error(pos, "[weak] is not valid on a lambda parameter. Copy the handle to a '[weak] T' local before "
                          "the lambda: the lambda captures it");
        else
            st.error(pos, "attribute '" + name + "' is not valid on a lambda parameter");
        return 0;
    }),
        lexy::parse_state, lexy::values);
};

// "Type name" or "name". A parameter without a type has an empty type ("auto").
struct param_p
{
    static constexpr auto rule = dsl::opt(dsl::peek(dsl::lit_c<'['>) >> dsl::p<lambda_attr_p>)
        + dsl::p<scan_param_type> + dsl::p<id_str> + dsl::if_(dsl::lit_c<','>);
    static constexpr auto value = lexy::bind(lexy::callback<ast::Param>(
                                                 [](const ParseState& st, auto, Span type, std::string name)
    {
        if (only_blank(span_text(st, type)))
            return ast::Param{std::move(name), ""};
        return ast::Param{std::move(name), type_of(st, type)};
    }),
        lexy::parse_state, lexy::values);
};

struct params_p
{
    static constexpr auto rule = dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::p<param_p>);
    static constexpr auto value = lexy::as_list<std::vector<ast::Param>>;
};

struct LambdaRest
{
    std::vector<ast::Param> params;
    Span body; // a block body: the span between the braces
    ExprP expr; // an expression body: the value of the lambda (else null)
};

// The part after "(": the parameters, "=>" and the body. The body is a block "{ ... }" or an
// expression. An expression body "e" is the same as the block "{ return e; }".
struct lambda_rest
{
    static constexpr auto rule = dsl::p<params_p>
        + LEXY_LIT("=>") + (dsl::peek(dsl::lit_c<'{'>) >> dsl::p<scan_balanced> | dsl::else_ >> dsl::recurse<expr>);
    static constexpr auto value =
        lexy::callback<LambdaRest>([](lexy::nullopt, Span body) { return LambdaRest{{}, body, nullptr}; },
            [](std::vector<ast::Param>&& ps, Span body) { return LambdaRest{std::move(ps), body, nullptr}; },
            [](lexy::nullopt, ExprP&& e) { return LambdaRest{{}, {}, std::move(e)}; },
            [](std::vector<ast::Param>&& ps, ExprP&& e) { return LambdaRest{std::move(ps), {}, std::move(e)}; });
};

// The text after "(" up to the matching ")". It has a fixed nesting depth of "(".
// Strings, characters and comments are skipped. A deeper nesting fails the scan.
template <int N> struct lambda_body : lexy::token_production
{
    static constexpr auto rule = []
    {
        auto head = dsl::lit_c<')'> >> dsl::break_ | dsl::eof >> dsl::error<e_targs> | dsl::token(string_lit)
            | dsl::token(char_lit) | line_comment | block_comment;
        if constexpr (N > 0)
            return dsl::loop(head | dsl::lit_c<'('> >> dsl::p<lambda_body<N - 1>> | dsl::else_ >> any_unit);
        else
            return dsl::loop(head | dsl::lit_c<'('> >> dsl::error<e_targs> | dsl::else_ >> any_unit);
    }();
    static constexpr auto value = lexy::noop;
};

// "(" ... ")" "=>" is a lambda. The "(" is already consumed.
constexpr auto lambda_head = dsl::p<lambda_body<lambda_depth>> + dsl::whitespace(ws_rule) + LEXY_LIT("=>");

// A parenthesized expression or an arrow lambda.
struct paren_p
{
    static constexpr auto rule = dsl::position + dsl::lit_c<'('>
        + (dsl::peek(lambda_head) >> dsl::p<lambda_rest> | dsl::else_ >> (dsl::recurse<expr> + dsl::lit_c<')'>));
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>([](const ParseState& st, const char* pos, ExprP&& inner)
    { return ast::make_expr(ast::ParenExpr{std::move(inner), st.offset(pos)}); },
                       [](const ParseState& st, const char* pos, LambdaRest&& rest)
    {
        ast::LambdaExpr l;
        l.params = std::move(rest.params);
        l.pos = st.offset(pos);
        if (rest.expr)
        {
            // "{ return e; }": the synthetic braces and "return" have no position.
            ast::ReturnStmt r;
            r.expr = std::move(rest.expr);
            l.body.stmts.push_back(ast::make_stmt(std::move(r)));
            return ast::make_expr(std::move(l));
        }
        const ParseInput in{std::string_view(st.base, st.size), st.map, st.array_tpl, st.aliases};
        l.body = parse_block(in, rest.body, *st.diag);
        return ast::make_expr(std::move(l));
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- operators with a rule
struct index_p
{
    static constexpr auto rule = dsl::recurse<expr> + dsl::lit_c<']'>;
    static constexpr auto value = lexy::forward<ExprP>;
};

// The middle of "a ? b : c", with the ":".
struct cond_mid
{
    static constexpr auto rule = dsl::recurse<expr> + dsl::lit_c<':'>;
    static constexpr auto value = lexy::forward<ExprP>;
};

// ---------------------------------------------------------------- cast
// A C-style cast "(T)e" is only a cast when the text in the parentheses is a type for sure:
//  - a builtin type word with the "*" and "&" marks and "const", for example "(int)", "(float*)",
//  - a name with at least one "*" or "&" mark, for example "(Foo*)" or "(const Foo&)".
// "(a * b)", "(x)" and "(int x) => ..." are not casts. The text "(a*)" is no expression.
// The operand of a cast is a unary expression: "(int)a + b" is "((int)a) + b".
constexpr auto cast_ws = dsl::whitespace(ws_rule);
#define HOT_CAST_WORD(W) (LEXY_KEYWORD(W, id) >> cast_ws)
constexpr auto cast_builtin = HOT_CAST_WORD("bool") | HOT_CAST_WORD("sbyte") | HOT_CAST_WORD("int8")
    | HOT_CAST_WORD("byte") | HOT_CAST_WORD("uint8") | HOT_CAST_WORD("short") | HOT_CAST_WORD("int16")
    | HOT_CAST_WORD("ushort") | HOT_CAST_WORD("uint16") | HOT_CAST_WORD("int") | HOT_CAST_WORD("int32")
    | HOT_CAST_WORD("uint") | HOT_CAST_WORD("uint32") | HOT_CAST_WORD("long") | HOT_CAST_WORD("int64")
    | HOT_CAST_WORD("ulong") | HOT_CAST_WORD("uint64") | HOT_CAST_WORD("nint") | HOT_CAST_WORD("intptr")
    | HOT_CAST_WORD("nuint") | HOT_CAST_WORD("uintptr") | HOT_CAST_WORD("float") | HOT_CAST_WORD("double")
    | HOT_CAST_WORD("char") | HOT_CAST_WORD("wchar") | HOT_CAST_WORD("char16");
#undef HOT_CAST_WORD
constexpr auto cast_mark = dsl::lit_c<'*'> / dsl::lit_c<'&'>;
constexpr auto cast_const = dsl::if_(kw_const >> cast_ws);
constexpr auto cast_marks = dsl::while_(cast_mark >> cast_ws);
constexpr auto cast_qname = dsl::token(id) + dsl::while_(dsl::lit_c<'.'> >> dsl::token(id));
constexpr auto cast_a = dsl::lit_c<'('> + cast_ws + cast_const + cast_builtin + cast_ws + cast_marks + dsl::lit_c<')'>;
constexpr auto cast_b =
    dsl::lit_c<'('> + cast_ws + cast_const + cast_qname + cast_ws + cast_mark + cast_ws + cast_marks + dsl::lit_c<')'>;

// A cast to an array type, for example "(int[3])x" or "(Foo[2])x". It is an error. A name needs
// an operand after the ")": "(a[3])" is an index expression.
constexpr auto cast_dims = dsl::lit_c<'['> + dsl::until(dsl::lit_c<']'>)
    + dsl::while_(dsl::lit_c<'['> >> dsl::until(dsl::lit_c<']'>));
constexpr auto cast_c = dsl::lit_c<'('> + cast_ws + cast_const + cast_builtin + cast_ws + cast_marks + cast_dims
    + cast_ws + dsl::lit_c<')'>;
constexpr auto cast_d = dsl::lit_c<'('> + cast_ws + cast_const + cast_qname + cast_ws + cast_marks + cast_dims
    + cast_ws + dsl::lit_c<')'> + cast_ws + dsl::peek(dsl::token(id_b) | dsl::ascii::digit);

// The text between the parentheses of a cast. It is already checked by cast_a or cast_b.
struct cast_scan : lexy::token_production
{
    static constexpr auto rule = dsl::position
        + dsl::loop(dsl::peek(dsl::lit_c<')'>) >> dsl::break_
            | dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> any_unit)
        + dsl::position;
    static constexpr auto value = span_value;
};
struct cast_atom_p
{
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'('> + dsl::p<cast_scan> + dsl::lit_c<')'> + dsl::recurse<unary_expr>;
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>([](const ParseState& st, const char* pos, Span type, ExprP&& e)
    { return ast::make_expr(ast::UnaryExpr{"(" + type_of(st, type) + ")", std::move(e), false, st.offset(pos)}); }),
            lexy::parse_state, lexy::values);
};

// The operand of the cast to an array type is read to find more errors. The result is the operand.
struct cast_array_p
{
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'('> + dsl::p<cast_scan> + dsl::lit_c<')'> + dsl::recurse<unary_expr>;
    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>([](const ParseState& st, const char* pos, Span type, ExprP&& e)
    {
        const std::string t(type_text(span_text(st, type)));
        st.error(pos, "cannot cast to the array type '" + t + "'");
        return std::move(e);
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- the expression
// "Full" is the whole expression. Not "Full" is the unary expression: a prefix operator or an
// atom with the postfix operators. A cast uses it for the operand.
template <bool Full> struct expr_g : lexy::expression_production
{
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_operator_nesting = 256;

    static constexpr auto atom = dsl::peek(dsl::if_(dsl::lit_c<'.'>) + dsl::ascii::digit) >> dsl::p<number_p>
        | dsl::peek(dsl::lit_c<'"'>) >> dsl::p<literal_p<ast::StringExpr, '"'>>
        | dsl::peek(dsl::lit_c<'\''>) >> dsl::p<literal_p<ast::CharExpr, '\''>>
        | dsl::peek(dsl::lit_c<'`'>) >> dsl::error<e_interpolation> | dsl::peek(cast_c) >> dsl::p<cast_array_p>
        | dsl::peek(cast_d) >> dsl::p<cast_array_p> | dsl::peek(cast_a) >> dsl::p<cast_atom_p>
        | dsl::peek(cast_b) >> dsl::p<cast_atom_p> | dsl::peek(dsl::lit_c<'('>) >> dsl::p<paren_p>
        | dsl::peek(dsl::lit_c<'{'>) >> dsl::error<e_brace_init> | dsl::peek(dsl::lit_c<'['>) >> dsl::p<coll_p>
        | dsl::peek(kw_this) >> dsl::p<kw_p<ast::ThisExpr, std::remove_cv_t<decltype(kw_this)>>>
        | dsl::peek(kw_base) >> dsl::p<kw_p<ast::BaseExpr, std::remove_cv_t<decltype(kw_base)>>>
        | dsl::peek(kw_null) >> dsl::p<kw_p<ast::NullExpr, std::remove_cv_t<decltype(kw_null)>>>
        | dsl::peek(kw_new) >> dsl::p<new_p> | dsl::peek(LEXY_LIT("::")) >> dsl::error<e_use_dot_colons>
        | dsl::peek(id_r) >> dsl::p<name_p> | dsl::else_ >> dsl::error<e_expected_expr>;

    // a++  a--  f(args)  a[i]  a.b. The text "a->b" is an error. The parse reports it at the
    // "->" and goes on, so a call or an index after it gives no second error.
    struct postfix_lvl : dsl::postfix_op
    {
        static constexpr auto op = HOT_POST("++") / HOT_POST("--")
            / dsl::op<CallTag>(LEXY_LIT("(") >> dsl::p<elems_p<')'>>)
            / dsl::op<IndexTag>(LEXY_LIT("[") >> dsl::p<index_p>)
            / dsl::op<SafeTag>(dsl::not_followed_by(LEXY_LIT("?."), dsl::ascii::digit) >> dsl::p<member_name_p>)
            / dsl::op<Member<HOT_CH3(".")>>(LEXY_LIT(".") >> dsl::p<member_name_p>)
            / dsl::op<ArrowTag>(LEXY_LIT("->") >> dsl::try_(dsl::error<e_use_dot_arrow>, dsl::p<id_str>));
        using operand = dsl::atom;
    };

    // - + ! ~ * & ++ -- await. The prefix "&" does not match "&&". A cast "(T)" is a prefix too.
    struct prefix_lvl : dsl::prefix_op
    {
        static constexpr auto op = HOT_PRE("++") / HOT_PRE("--") / HOT_PRE_NF("-", cc_minus_only)
            / HOT_PRE_NF("+", cc_plus_only) / HOT_PRE("!") / HOT_PRE("~") / HOT_PRE("*") / HOT_PRE_NF("&", cc_amp_only)
            / dsl::op<AwaitTag>(kw_await);
        using operand = postfix_lvl;
    };

    struct mul_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("*", cc_eq) / HOT_BIN_NF("/", cc_eq) / HOT_BIN_NF("%", cc_eq);
        using operand = prefix_lvl;
    };
    struct add_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("+", cc_plus) / HOT_BIN_NF("-", cc_minus);
        using operand = mul_lvl;
    };
    struct shift_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("<<", cc_eq) / HOT_BIN_NF(">>", cc_eq);
        using operand = add_lvl;
    };
    // "obj is T" and "obj as T". They bind tighter than "<" and looser than "<<".
    struct test_lvl : dsl::postfix_op
    {
        static constexpr auto op =
            dsl::op<IsTag>(kw_is >> dsl::p<qname_tok> + dsl::if_(dsl::peek(dsl::lit_c<'['>) >> dsl::error<e_test_array>)
                + dsl::opt(dsl::peek(id_b) >> dsl::p<bind_tok>))
            / dsl::op<AsTag>(LEXY_KEYWORD("as", id) >> dsl::p<qname_tok>
                + dsl::if_(dsl::peek(dsl::lit_c<'['>) >> dsl::error<e_test_array>));
        using operand = shift_lvl;
    };
    struct rel_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("<", cc_lt) / HOT_BIN("<=") / HOT_BIN_NF(">", cc_gt) / HOT_BIN(">=");
        using operand = test_lvl;
    };
    struct eq_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN("==") / HOT_BIN("!=");
        using operand = rel_lvl;
    };
    struct band_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("&", cc_amp);
        using operand = eq_lvl;
    };
    struct bxor_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("^", cc_eq);
        using operand = band_lvl;
    };
    struct bor_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN_NF("|", cc_bar);
        using operand = bxor_lvl;
    };
    struct land_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN("&&");
        using operand = bor_lvl;
    };
    struct lor_lvl : dsl::infix_op_left
    {
        static constexpr auto op = HOT_BIN("||");
        using operand = land_lvl;
    };
    // "x ?? y": binds looser than "||" and tighter than "?:". It is right associative.
    struct coalesce_lvl : dsl::infix_op_right
    {
        static constexpr auto op = HOT_BIN("??");
        using operand = lor_lvl;
    };
    struct cond_lvl : dsl::infix_op_right
    {
        static constexpr auto op = dsl::op<CondTag>(LEXY_LIT("?") >> dsl::p<cond_mid>);
        using operand = coalesce_lvl;
    };
    struct assign_lvl : dsl::infix_op_right
    {
        static constexpr auto op = HOT_BIN_NF("=", cc_eq) / HOT_BIN("+=") / HOT_BIN("-=") / HOT_BIN("*=")
            / HOT_BIN("/=") / HOT_BIN("%=") / HOT_BIN("<<=") / HOT_BIN(">>=") / HOT_BIN("&=") / HOT_BIN("|=")
            / HOT_BIN("^=");
        using operand = cond_lvl;
    };

    using operation = std::conditional_t<Full, assign_lvl, prefix_lvl>;

    static constexpr auto value = lexy::bind(lexy::callback<ExprP>(Builder{}), lexy::parse_state, lexy::values);
};

struct expr : expr_g<true>
{
};
struct unary_expr : expr_g<false>
{
};

// ---------------------------------------------------------------- the entry points
struct ExprResult
{
    ExprP expr;
    const char* end = nullptr;
};

struct expr_entry
{
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_recursion_depth = max_depth;
    static constexpr auto rule = dsl::whitespace(ws_rule) + dsl::p<expr> + dsl::position;
    static constexpr auto value =
        lexy::callback<ExprResult>([](ExprP&& e, const char* end) { return ExprResult{std::move(e), end}; });
};

struct list_body
{
    static constexpr auto rule = dsl::list(dsl::p<expr>, dsl::sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<ExprP>>;
};

struct ListResult
{
    std::vector<ExprP> items;
    const char* end = nullptr;
};

struct list_entry
{
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_recursion_depth = max_depth;
    static constexpr auto rule =
        dsl::whitespace(ws_rule) + dsl::opt(dsl::peek_not(dsl::eof) >> dsl::p<list_body>) + dsl::position;
    static constexpr auto value = lexy::callback<ListResult>([](lexy::nullopt, const char* end) {
        return ListResult{{}, end};
    }, [](std::vector<ExprP>&& v, const char* end) { return ListResult{std::move(v), end}; });
};

// The result of a parse and the count of the errors that lexy found.
template <typename Entry, typename Result>
bool run(ParseInput in, Span span, Diagnostics& diag, Result& out, uint32_t& end_offset)
{
    const std::size_t size = in.text.size();
    const std::size_t end = span.end < size ? span.end : size;
    const std::size_t begin = span.begin < end ? span.begin : end;
    const ParseState st{in.text.data(), in.map, &diag, size, in.array_tpl, in.aliases};
    const auto input = lexy::string_input<Encoding>(in.text.data() + begin, end - begin);
    int syntax_errors = 0;
    auto sink = lexy::callback<void>([&](const auto&, const auto& e)
    {
        ++syntax_errors;
        detail::report(st, e);
    });
    auto result = lexy::parse<Entry>(input, st, sink);
    if (syntax_errors != 0 || !result.has_value())
        return false;
    out = std::move(result).value();
    end_offset = st.offset(out.end);
    // The end of the parse must be the end of the span.
    if (end_offset != end)
    {
        diag.add(end_offset, *in.map, e_end_of_expr::name);
        return false;
    }
    return true;
}

} // namespace

ast::ExprP parse_expression(ParseInput in, Span span, Diagnostics& diag)
{
    ExprResult r;
    uint32_t end = 0;
    if (!run<expr_entry>(in, span, diag, r, end))
        return nullptr;
    return std::move(r.expr);
}

std::vector<ast::ExprP> parse_expression_list(ParseInput in, Span span, Diagnostics& diag)
{
    ListResult r;
    uint32_t end = 0;
    if (!run<list_entry>(in, span, diag, r, end))
        return {};
    return std::move(r.items);
}

} // namespace hotc
