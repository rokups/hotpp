// The grammar of the expressions.
//
// The grammar is one lexy expression_production. It builds the Expr nodes of ast.h.
// A parent grammar scans the span of an expression and calls parse_expression. The
// function parses the span in the same buffer, so each position is an absolute offset.
//
// Two rules need a look-ahead and not a recursion, because dsl::peek cannot recurse:
//  - A "(" starts a lambda when the text after it is ")" "=>" or "name :". A lambda
//    that has no "=>" gives an error of the lambda path, not an error of the paren path.
//    The text "(" name ")" "=>" gives the error "a lambda parameter needs the form 'name: type'".
//  - A "<" starts template arguments when a scan of "<...>" ends before a "(", a "{" or
//    a "::". The scan has a fixed nesting depth (targs_depth). It stops at a newline,
//    at ";" and at each char that a type cannot have.
#include "expression.h"

#include <cstddef>
#include <string>
#include <vector>

#include "lexy_common.hpp"
#include "statement.h"

namespace spc {
namespace {

using namespace parser;  // NOLINT: the grammar uses the names of lexy_common.hpp
using ast::ExprP;

// ---------------------------------------------------------------- error texts
SPC_ERROR_TAG(e_expected_expr, "expected an expression");
SPC_ERROR_TAG(e_end_of_expr, "expected end of expression");
SPC_ERROR_TAG(e_lambda_param, "a lambda parameter needs the form 'name: type'");
SPC_ERROR_TAG(e_targs, "not a template argument list");

// ---------------------------------------------------------------- limits
// The nesting of the brackets in the text of template arguments.
constexpr int targs_depth = 4;
// The limit of dsl::recurse: nested "(", "[", "{", "?" and argument lists. Each level
// uses some stack, so the limit is low. A script does not nest deeper.
constexpr std::size_t max_depth = 256;

// ---------------------------------------------------------------- words
constexpr auto kw_let = LEXY_KEYWORD("let", id);
constexpr auto kw_const = LEXY_KEYWORD("const", id);
constexpr auto kw_this = LEXY_KEYWORD("this", id);
constexpr auto kw_super = LEXY_KEYWORD("super", id);
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

// A name. The words of the body syntax are not names.
constexpr auto id_r = id.reserve(kw_let, kw_const, kw_this, kw_super, kw_null, kw_if, kw_else,
                                 kw_while, kw_do, kw_for, kw_return, kw_break, kw_continue,
                                 kw_switch, kw_case, kw_default);

// ---------------------------------------------------------------- plain helpers
std::string_view span_text(const ParseState& st, Span sp)
{
    return std::string_view(st.base + sp.begin, sp.end - sp.begin);
}

std::string type_of(const ParseState& st, Span sp)
{
    if (only_blank(span_text(st, sp))) st.error(st.base + sp.begin, "expected a type");
    return type_text(span_text(st, sp));
}

// Splits the raw text of a qualified name. The text is "::"? part ("::" part)*. A part
// can have template arguments. The arguments of the last part go to template_args. The
// arguments of the other parts stay in the text of the part, for example "A<int>".
// The arguments of the last part are also parsed as a list of expressions, when that works.
// The try is quiet: a failed try records no error. The text stays in template_args.
ast::NameExpr split_name(const ParseState& st, std::string_view t, uint32_t pos)
{
    ast::NameExpr n;
    n.pos = pos;
    std::size_t i = 0;
    if (t.substr(0, 2) == "::") {
        n.leading_colons = true;
        i = 2;
    }
    while (i < t.size()) {
        const std::size_t b = i;
        while (i < t.size() && is_id(t[i])) ++i;
        std::string part(t.substr(b, i - b));
        if (i < t.size() && t[i] == '<') {
            int depth = 0;
            std::size_t j = i;
            for (; j < t.size(); ++j) {
                if (t[j] == '<')
                    ++depth;
                else if (t[j] == '>' && --depth == 0)
                    break;
            }
            const std::size_t e = j < t.size() ? j + 1 : t.size();
            const std::string targs(t.substr(i, e - i));
            i = e;
            if (i >= t.size()) {
                n.template_args = targs;
                if (targs.size() > 2) {
                    const uint32_t b = pos + static_cast<uint32_t>(e - targs.size()) + 1;
                    const Span args{b, b + static_cast<uint32_t>(targs.size()) - 2};
                    Diagnostics quiet;
                    std::vector<ExprP> list = parse_expression_list(
                        ParseInput{std::string_view(st.base, st.size), st.map}, args, quiet);
                    if (quiet.items.empty() && !list.empty()) {
                        n.targ_exprs = std::move(list);
                        n.targ_parsed = true;
                    }
                }
            } else
                part += targs;
        }
        n.parts.push_back(std::move(part));
        if (t.substr(i, 2) != "::") break;
        i += 2;
    }
    return n;
}

// ---------------------------------------------------------------- operator tags
// A string as a template argument. It names an operator.
template <std::size_t N>
struct Str {
    char v[N] = {};
    constexpr Str(const char (&s)[N])
    {
        for (std::size_t i = 0; i < N; ++i) v[i] = s[i];
    }
};

template <Str S>
struct Bin {};  // an infix operator, also an assignment
template <Str S>
struct Post {};  // "++" or "--" after the operand
template <Str S>
struct Member {};  // "." or "->"
template <Str S>
struct Pre {  // a prefix operator; it keeps the position of the operator
    const char* pos;
    explicit Pre(const char* p) : pos(p) {}
};
struct CondTag {};
struct CallTag {};
struct IndexTag {};

#define SPC_BIN(S) dsl::op<Bin<S>>(LEXY_LIT(S))
#define SPC_BIN_NF(S, CC) dsl::op<Bin<S>>(dsl::not_followed_by(LEXY_LIT(S), CC))
#define SPC_PRE(S) dsl::op<Pre<S>>(LEXY_LIT(S))
#define SPC_PRE_NF(S, CC) dsl::op<Pre<S>>(dsl::not_followed_by(LEXY_LIT(S), CC))
#define SPC_POST(S) dsl::op<Post<S>>(LEXY_LIT(S))

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

// ---------------------------------------------------------------- the nodes
// The value of an expression production. Each atom production gives an ExprP.
// The first argument is the parse state. Only the prefix operators use it.
struct Builder {
    ExprP operator()(const ParseState&, ExprP e) const { return e; }

    template <Str S>
    ExprP operator()(const ParseState&, ExprP l, Bin<S>, ExprP r) const
    {
        const uint32_t pos = ast::pos_of(*l);
        return ast::make_expr(ast::BinaryExpr{S.v, std::move(l), std::move(r), pos});
    }
    template <Str S>
    ExprP operator()(const ParseState& st, Pre<S> t, ExprP e) const
    {
        return ast::make_expr(ast::UnaryExpr{S.v, std::move(e), false, st.offset(t.pos)});
    }
    template <Str S>
    ExprP operator()(const ParseState&, ExprP e, Post<S>) const
    {
        const uint32_t pos = ast::pos_of(*e);
        return ast::make_expr(ast::UnaryExpr{S.v, std::move(e), true, pos});
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
    template <Str S>
    ExprP operator()(const ParseState&, ExprP o, Member<S>, std::string name) const
    {
        const uint32_t pos = ast::pos_of(*o);
        return ast::make_expr(ast::MemberExpr{std::move(o), std::move(name), S.v[0] == '-', pos});
    }
};

// ---------------------------------------------------------------- tokens
struct id_str : lexy::token_production {
    static constexpr auto rule = id;
    static constexpr auto value = lexy::as_string<std::string>;
};

// A number. The rules are the rules of the old lexer. The text stays raw.
// A sign belongs to the number after an "e" or an "E", but not in a hex number.
constexpr auto num_word = dsl::ascii::word / dsl::lit_c<'.'>;
constexpr auto num_exp =
    (dsl::lit_c<'e'> / dsl::lit_c<'E'>) >> dsl::if_(dsl::lit_c<'+'> / dsl::lit_c<'-'>);
constexpr auto num_hex = dsl::literal_set(LEXY_LIT("0x"), LEXY_LIT("0X")) >> dsl::while_(num_word);

struct number_p : lexy::token_production {
    static constexpr auto rule =
        dsl::position + (num_hex | dsl::else_ >> dsl::while_(num_exp | num_word)) + dsl::position;
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>([](const ParseState& st, const char* b, const char* e) {
            return ast::make_expr(ast::NumberExpr{std::string(b, e), st.offset(b)});
        }),
        lexy::parse_state, lexy::values);
};

// A string or a character literal. The text keeps the quotes.
template <typename Node, char Quote>
struct literal_p : lexy::token_production {
    static constexpr auto rule = dsl::position + quoted<Quote> + dsl::position;
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>([](const ParseState& st, const char* b, const char*, const char*,
                                 const char* e) {
            return ast::make_expr(Node{std::string(b, e), st.offset(b)});
        }),
        lexy::parse_state, lexy::values);
};

// this, super, null
template <typename Node, typename Kw>
struct kw_p : lexy::token_production {
    static constexpr auto rule = dsl::position + dsl::token(Kw{});
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>([](const ParseState& st, const char* b) {
            return ast::make_expr(Node{st.offset(b)});
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- template arguments
// The text after "<" up to the matching ">". It has a fixed nesting depth. A char that
// a type cannot have fails the scan, so "a < b && c > (d)" is not a template call.
template <int N>
struct targs_body : lexy::token_production {
    static constexpr auto rule = [] {
        auto plain = dsl::ascii::alnum / dsl::lit_c<'_'> / dsl::lit_c<' '> / dsl::lit_c<'\t'> /
                     dsl::lit_c<','> / dsl::lit_c<'*'> / dsl::lit_c<'.'> / dsl::lit_c<'&'>;
        auto head = dsl::lit_c<'>'> >> dsl::break_ | LEXY_LIT("&&") >> dsl::error<e_targs> |
                    LEXY_LIT("::") | plain;
        if constexpr (N > 0)
            return dsl::loop(head | dsl::lit_c<'<'> >> dsl::p<targs_body<N - 1>> |
                             dsl::else_ >> dsl::error<e_targs>);
        else
            return dsl::loop(head | dsl::else_ >> dsl::error<e_targs>);
    }();
    static constexpr auto value = lexy::noop;
};

struct targs_tok : lexy::token_production {
    static constexpr auto rule = dsl::lit_c<'<'> + dsl::p<targs_body<targs_depth>>;
    static constexpr auto value = lexy::noop;
};

// "<...>" that is followed by "(", "{" or "::".
constexpr auto targs_head =
    dsl::lit_c<'<'> + dsl::p<targs_body<targs_depth>> +
    dsl::while_(dsl::lit_c<' '> / dsl::lit_c<'\t'>) +
    dsl::literal_set(dsl::lit_c<'('>, dsl::lit_c<'{'>, LEXY_LIT("::"));

// ---------------------------------------------------------------- names
// The value is the span of the whole name, with the template arguments.
struct qname_tok : lexy::token_production {
    static constexpr auto rule = [] {
        auto seg = dsl::token(id_r) + dsl::if_(dsl::peek(targs_head) >> dsl::p<targs_tok>);
        return dsl::position + dsl::if_(LEXY_LIT("::")) + seg +
               dsl::loop(dsl::peek(LEXY_LIT("::") + id_r) >> (LEXY_LIT("::") + seg) |
                         dsl::else_ >> dsl::break_) +
               dsl::position;
    }();
    static constexpr auto value = span_value;
};

// The elements of a list up to the closer. The opener is already read.
template <char Close>
struct elems_p {
    static constexpr auto rule =
        dsl::opt(dsl::peek_not(dsl::lit_c<Close>) >>
                 dsl::list(dsl::recurse<expr>, dsl::sep(dsl::comma))) +
        dsl::lit_c<Close>;
    static constexpr auto value = lexy::as_list<std::vector<ExprP>>;
};

// A name, with an optional list of braced elements: "Vec3{1, 2, 3}".
struct name_p {
    static constexpr auto rule =
        dsl::p<qname_tok> +
        dsl::opt(dsl::peek(dsl::lit_c<'{'>) >> (dsl::lit_c<'{'> + dsl::p<elems_p<'}'>>));
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>(
            [](const ParseState& st, Span name, lexy::nullopt) {
                return ast::make_expr(split_name(st, span_text(st, name), name.begin));
            },
            [](const ParseState& st, Span name, std::vector<ExprP>&& elems) {
                return ast::make_expr(ast::TypedBracedExpr{std::string(span_text(st, name)),
                                                           std::move(elems), name.begin});
            }),
        lexy::parse_state, lexy::values);
};

struct braced_p {
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'{'> + dsl::p<elems_p<'}'>>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>([](const ParseState& st, const char* pos, std::vector<ExprP>&& e) {
            return ast::make_expr(ast::BracedListExpr{std::move(e), st.offset(pos)});
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- lambda
struct param_term {
    static constexpr auto term = dsl::lit_c<','> / dsl::lit_c<')'>;
};

struct param_p {
    static constexpr auto rule = dsl::p<id_str> + dsl::lit_c<':'> +
                                 dsl::p<scan_type<param_term>> + dsl::if_(dsl::lit_c<','>);
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Param>([](const ParseState& st, std::string name, Span type) {
            return ast::Param{std::move(name), type_of(st, type)};
        }),
        lexy::parse_state, lexy::values);
};

struct params_p {
    static constexpr auto rule =
        dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::p<param_p>);
    static constexpr auto value = lexy::as_list<std::vector<ast::Param>>;
};

struct LambdaRest {
    std::vector<ast::Param> params;
    Span body;
};

// The part after "(": the parameters, "=>" and the body.
struct lambda_rest {
    static constexpr auto rule =
        dsl::p<params_p> + LEXY_LIT("=>") + dsl::p<scan_balanced>;
    static constexpr auto value = lexy::callback<LambdaRest>(
        [](lexy::nullopt, Span body) { return LambdaRest{{}, body}; },
        [](std::vector<ast::Param>&& ps, Span body) { return LambdaRest{std::move(ps), body}; });
};

// "(" ")" "=>" or "(" name ":" is a lambda. The name is not followed by another ":".
constexpr auto lambda_empty = dsl::lit_c<')'> + dsl::whitespace(ws_rule) + LEXY_LIT("=>");
constexpr auto lambda_param =
    id + dsl::whitespace(ws_rule) + dsl::lit_c<':'> + dsl::peek_not(dsl::lit_c<':'>);
// "(" name ")" "=>" is a lambda with a parameter that has no type. It is an error.
constexpr auto lambda_untyped =
    id + dsl::whitespace(ws_rule) + dsl::lit_c<')'> + dsl::whitespace(ws_rule) + LEXY_LIT("=>");

// A parenthesized expression or an arrow lambda.
struct paren_p {
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'('> +
        (dsl::peek(lambda_empty) >> dsl::p<lambda_rest> |
         dsl::peek(lambda_param) >> dsl::p<lambda_rest> |
         dsl::peek(lambda_untyped) >> dsl::error<e_lambda_param> |
         dsl::else_ >> (dsl::recurse<expr> + dsl::lit_c<')'>));
    static constexpr auto value = lexy::bind(
        lexy::callback<ExprP>(
            [](const ParseState& st, const char* pos, ExprP&& inner) {
                return ast::make_expr(ast::ParenExpr{std::move(inner), st.offset(pos)});
            },
            [](const ParseState& st, const char* pos, LambdaRest&& rest) {
                ast::LambdaExpr l;
                l.params = std::move(rest.params);
                l.pos = st.offset(pos);
                const ParseInput in{std::string_view(st.base, st.size), st.map};
                l.body = parse_block(in, rest.body, *st.diag);
                return ast::make_expr(std::move(l));
            }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- operators with a rule
struct index_p {
    static constexpr auto rule = dsl::recurse<expr> + dsl::lit_c<']'>;
    static constexpr auto value = lexy::forward<ExprP>;
};

// The middle of "a ? b : c", with the ":".
struct cond_mid {
    static constexpr auto rule = dsl::recurse<expr> + dsl::lit_c<':'>;
    static constexpr auto value = lexy::forward<ExprP>;
};

// ---------------------------------------------------------------- the expression
struct expr : lexy::expression_production {
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_operator_nesting = 256;

    static constexpr auto atom =
        dsl::peek(dsl::if_(dsl::lit_c<'.'>) + dsl::ascii::digit) >> dsl::p<number_p> |
        dsl::peek(dsl::lit_c<'"'>) >> dsl::p<literal_p<ast::StringExpr, '"'>> |
        dsl::peek(dsl::lit_c<'\''>) >> dsl::p<literal_p<ast::CharExpr, '\''>> |
        dsl::peek(dsl::lit_c<'`'>) >> dsl::error<e_interpolation> |
        dsl::peek(dsl::lit_c<'('>) >> dsl::p<paren_p> |
        dsl::peek(dsl::lit_c<'{'>) >> dsl::p<braced_p> |
        dsl::peek(kw_this) >> dsl::p<kw_p<ast::ThisExpr, std::remove_cv_t<decltype(kw_this)>>> |
        dsl::peek(kw_super) >> dsl::p<kw_p<ast::SuperExpr, std::remove_cv_t<decltype(kw_super)>>> |
        dsl::peek(kw_null) >> dsl::p<kw_p<ast::NullExpr, std::remove_cv_t<decltype(kw_null)>>> |
        dsl::peek(dsl::if_(LEXY_LIT("::")) + id_r) >> dsl::p<name_p> |
        dsl::else_ >> dsl::error<e_expected_expr>;

    // a++  a--  f(args)  a[i]  a.b  a->b
    struct postfix_lvl : dsl::postfix_op {
        static constexpr auto op =
            SPC_POST("++") / SPC_POST("--") /
            dsl::op<CallTag>(LEXY_LIT("(") >> dsl::p<elems_p<')'>>) /
            dsl::op<IndexTag>(LEXY_LIT("[") >> dsl::p<index_p>) /
            dsl::op<Member<".">>(LEXY_LIT(".") >> dsl::p<id_str>) /
            dsl::op<Member<"->">>(LEXY_LIT("->") >> dsl::p<id_str>);
        using operand = dsl::atom;
    };

    // - + ! ~ & * ++ --
    struct prefix_lvl : dsl::prefix_op {
        static constexpr auto op =
            SPC_PRE("++") / SPC_PRE("--") / SPC_PRE_NF("-", cc_minus_only) /
            SPC_PRE_NF("+", cc_plus_only) / SPC_PRE("!") / SPC_PRE("~") / SPC_PRE("*") /
            SPC_PRE_NF("&", cc_amp_only);
        using operand = postfix_lvl;
    };

    struct mul_lvl : dsl::infix_op_left {
        static constexpr auto op =
            SPC_BIN_NF("*", cc_eq) / SPC_BIN_NF("/", cc_eq) / SPC_BIN_NF("%", cc_eq);
        using operand = prefix_lvl;
    };
    struct add_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN_NF("+", cc_plus) / SPC_BIN_NF("-", cc_minus);
        using operand = mul_lvl;
    };
    struct shift_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN_NF("<<", cc_eq) / SPC_BIN_NF(">>", cc_eq);
        using operand = add_lvl;
    };
    struct rel_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN_NF("<", cc_lt) / SPC_BIN("<=") /
                                   SPC_BIN_NF(">", cc_gt) / SPC_BIN(">=");
        using operand = shift_lvl;
    };
    struct eq_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN("==") / SPC_BIN("!=");
        using operand = rel_lvl;
    };
    struct band_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN_NF("&", cc_amp);
        using operand = eq_lvl;
    };
    struct bxor_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN_NF("^", cc_eq);
        using operand = band_lvl;
    };
    struct bor_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN_NF("|", cc_bar);
        using operand = bxor_lvl;
    };
    struct land_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN("&&");
        using operand = bor_lvl;
    };
    struct lor_lvl : dsl::infix_op_left {
        static constexpr auto op = SPC_BIN("||");
        using operand = land_lvl;
    };
    struct cond_lvl : dsl::infix_op_right {
        static constexpr auto op = dsl::op<CondTag>(LEXY_LIT("?") >> dsl::p<cond_mid>);
        using operand = lor_lvl;
    };
    struct assign_lvl : dsl::infix_op_right {
        static constexpr auto op =
            SPC_BIN_NF("=", cc_eq) / SPC_BIN("+=") / SPC_BIN("-=") / SPC_BIN("*=") /
            SPC_BIN("/=") / SPC_BIN("%=") / SPC_BIN("<<=") / SPC_BIN(">>=") / SPC_BIN("&=") /
            SPC_BIN("|=") / SPC_BIN("^=");
        using operand = cond_lvl;
    };

    using operation = assign_lvl;

    static constexpr auto value =
        lexy::bind(lexy::callback<ExprP>(Builder{}), lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- the entry points
struct ExprResult {
    ExprP expr;
    const char* end = nullptr;
};

struct expr_entry {
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_recursion_depth = max_depth;
    static constexpr auto rule = dsl::whitespace(ws_rule) + dsl::p<expr> + dsl::position;
    static constexpr auto value = lexy::callback<ExprResult>(
        [](ExprP&& e, const char* end) { return ExprResult{std::move(e), end}; });
};

struct list_body {
    static constexpr auto rule = dsl::list(dsl::p<expr>, dsl::sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<ExprP>>;
};

struct ListResult {
    std::vector<ExprP> items;
    const char* end = nullptr;
};

struct list_entry {
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_recursion_depth = max_depth;
    static constexpr auto rule =
        dsl::whitespace(ws_rule) +
        dsl::opt(dsl::peek_not(dsl::eof) >> dsl::p<list_body>) + dsl::position;
    static constexpr auto value = lexy::callback<ListResult>(
        [](lexy::nullopt, const char* end) { return ListResult{{}, end}; },
        [](std::vector<ExprP>&& v, const char* end) { return ListResult{std::move(v), end}; });
};

// The result of a parse and the count of the errors that lexy found.
template <typename Entry, typename Result>
bool run(ParseInput in, Span span, Diagnostics& diag, Result& out, uint32_t& end_offset)
{
    const std::size_t size = in.text.size();
    const std::size_t end = span.end < size ? span.end : size;
    const std::size_t begin = span.begin < end ? span.begin : end;
    const ParseState st{in.text.data(), in.map, &diag, size};
    const auto input = lexy::string_input<Encoding>(in.text.data() + begin, end - begin);
    int syntax_errors = 0;
    auto sink = lexy::callback<void>([&](const auto&, const auto& e) {
        ++syntax_errors;
        detail::report(st, e);
    });
    auto result = lexy::parse<Entry>(input, st, sink);
    if (syntax_errors != 0 || !result.has_value()) return false;
    out = std::move(result).value();
    end_offset = st.offset(out.end);
    // The end of the parse must be the end of the span.
    if (end_offset != end) {
        diag.add(end_offset, *in.map, e_end_of_expr::name);
        return false;
    }
    return true;
}

}  // namespace

ast::ExprP parse_expression(ParseInput in, Span span, Diagnostics& diag)
{
    ExprResult r;
    uint32_t end = 0;
    if (!run<expr_entry>(in, span, diag, r, end)) return nullptr;
    return std::move(r.expr);
}

std::vector<ast::ExprP> parse_expression_list(ParseInput in, Span span, Diagnostics& diag)
{
    ListResult r;
    uint32_t end = 0;
    if (!run<list_entry>(in, span, diag, r, end)) return {};
    return std::move(r.items);
}

}  // namespace spc
