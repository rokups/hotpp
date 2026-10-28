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
//    "of" is a keyword only in the header of a for-of statement.
//  - An expression span ends at a terminator at depth 0. The depth counts (), [] and {}.
//    Strings, characters and comments are skipped.
//  - The label of a "case" ends at the first ":" at depth 0. The script has no "::".
//    A bare ternary "a ? b : c" in a label is not supported, because its ":" ends the
//    label. Put the ternary in parentheses.
//  - A catch clause is "catch (name: type)" or "catch (...)". A clause with a type and no
//    name is an error.
//  - A for statement is a for-of statement when dsl::peek finds
//    "( (let|const) name (: type)? of". Other for statements are classic.
//  - The init of a classic for is one declaration or a list of expressions.
//
// Error recovery: a broken statement skips to the next ";" (consumed) or to the "}" that
// ends the block (not consumed). The next statements still parse. A statement with an
// error in an expression is not in the result. The error is in the diagnostics.
//
// Nesting: lexy limits the nesting of statements and brackets inside one block. A
// thread-local counter limits the nesting of parse_block calls (lambda in lambda).
#include "statement.h"

#include <string>
#include <utility>
#include <vector>

#include "expression.h"
#include "lexy_common.hpp"

namespace spc {
namespace {

using namespace parser;  // NOLINT: the grammar uses the names of lexy_common.hpp
using ast::ExprP;
using ast::StmtP;

// ---------------------------------------------------------------- limits
// The limit of dsl::recurse in one block: nested statements and brackets.
constexpr std::size_t max_depth = 64;
// The limit of nested parse_block calls: a lambda in a lambda.
constexpr int max_block_nesting = 16;

// ---------------------------------------------------------------- error texts
SPC_ERROR_TAG(e_stmt, "expected a statement");
SPC_ERROR_TAG(e_while, "expected 'while'");
SPC_ERROR_TAG(e_name, "expected a name");
SPC_ERROR_TAG(e_catch, "expected 'catch'");

// ---------------------------------------------------------------- words
constexpr auto kw_let = LEXY_KEYWORD("let", id);
constexpr auto kw_const = LEXY_KEYWORD("const", id);
constexpr auto kw_if = LEXY_KEYWORD("if", id);
constexpr auto kw_else = LEXY_KEYWORD("else", id);
constexpr auto kw_while = LEXY_KEYWORD("while", id);
constexpr auto kw_do = LEXY_KEYWORD("do", id);
constexpr auto kw_for = LEXY_KEYWORD("for", id);
constexpr auto kw_of = LEXY_KEYWORD("of", id);
constexpr auto kw_return = LEXY_KEYWORD("return", id);
constexpr auto kw_break = LEXY_KEYWORD("break", id);
constexpr auto kw_continue = LEXY_KEYWORD("continue", id);
constexpr auto kw_switch = LEXY_KEYWORD("switch", id);
constexpr auto kw_case = LEXY_KEYWORD("case", id);
constexpr auto kw_default = LEXY_KEYWORD("default", id);
constexpr auto kw_try = LEXY_KEYWORD("try", id);
constexpr auto kw_catch = LEXY_KEYWORD("catch", id);
constexpr auto kw_throw = LEXY_KEYWORD("throw", id);

constexpr auto let_or_const = dsl::literal_set(kw_let, kw_const);

// ---------------------------------------------------------------- plain helpers
std::string_view span_text(const ParseState& st, Span sp)
{
    return std::string_view(st.base + sp.begin, sp.end - sp.begin);
}

bool blank(const ParseState& st, Span sp) { return only_blank(span_text(st, sp)); }

// The type text of a span. A "." is a scope and becomes "::". A "::" is an error.
std::string type_of(const ParseState& st, Span sp) { return type_of_span(st, sp); }

ParseInput input_of(const ParseState& st)
{
    return ParseInput{std::string_view(st.base, st.size), st.map};
}

// The expression of the span. Null: the error is in the diagnostics.
ExprP expr_of(const ParseState& st, Span sp)
{
    return parse_expression(input_of(st), sp, *st.diag);
}

// The expression list of the span. False: the error is in the diagnostics.
bool list_of(const ParseState& st, Span sp, std::vector<ExprP>& out)
{
    if (blank(st, sp)) return true;
    const std::size_t before = st.diag->items.size();
    out = parse_expression_list(input_of(st), sp, *st.diag);
    return st.diag->items.size() == before;
}

// ---------------------------------------------------------------- values
struct VarHead {
    uint32_t pos = 0;
    bool is_const = false;
    std::string name, type;
};

struct DeclParts {
    VarHead head;
    bool has_init = false;
    Span init;
};

struct ForInit {
    bool is_decl = false;
    DeclParts decl;
    Span span;  // the expressions, when is_decl is false
};

struct ForRest {
    ForInit init;
    Span cond, step;
    StmtP body;
};

struct ForOfRest {
    VarHead head;
    Span range;
    StmtP body;
};

struct ElsePart {
    uint32_t pos = 0;
    StmtP stmt;
};

struct SwItem {
    enum Kind { None, Case, Default, Statement } kind = None;
    ExprP label;  // Case: null when the label has an error
    uint32_t pos = 0;
    StmtP stmt;
};

StmtP make_decl(const ParseState& st, DeclParts&& d)
{
    ast::VarDeclStmt v;
    v.is_const = d.head.is_const;
    v.name = std::move(d.head.name);
    v.type = std::move(d.head.type);
    v.pos = d.head.pos;
    if (d.has_init) {
        v.init = expr_of(st, d.init);
        if (!v.init) return nullptr;
    }
    return ast::make_stmt(std::move(v));
}

std::vector<StmtP> keep(std::vector<StmtP>&& v)
{
    std::vector<StmtP> out;
    for (StmtP& s : v)
        if (s) out.push_back(std::move(s));
    return out;
}

// ---------------------------------------------------------------- tokens
// A name. The value is the text.
struct name_p : lexy::token_production {
    static constexpr auto rule = dsl::must(id).template error<e_name>;
    static constexpr auto value = lexy::as_string<std::string>;
};

// ---------------------------------------------------------------- terminators
constexpr auto end_of_stmt = dsl::eof | (dsl::lit_c<';'> / dsl::lit_c<'}'>);
constexpr auto end_of_part = dsl::eof | (dsl::lit_c<';'> / dsl::lit_c<')'> / dsl::lit_c<'}'>);

// The expression of a statement.
struct expr_cfg {
    static constexpr auto open = dsl::whitespace(ws_rule);
    static constexpr auto term = end_of_stmt;
};
// The "= expression" of a declaration.
struct init_cfg {
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = end_of_stmt;
};
// The "( expression" of if, while, do-while and switch. The ")" stays in the input.
struct cond_cfg {
    static constexpr auto open = dsl::lit_c<'('>;
    static constexpr auto term = end_of_part;
};
// A part of the header of a for statement.
struct part_cfg {
    static constexpr auto open = dsl::whitespace(ws_rule);
    static constexpr auto term = end_of_part;
};
struct for_init_cfg {
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = end_of_part;
};
// The type of a declaration.
struct decl_type_cfg {
    static constexpr auto term = dsl::lit_c<'='> | end_of_stmt;
};
struct for_type_cfg {
    static constexpr auto term = dsl::lit_c<'='> | end_of_part;
};
// The type in a for-of header. White space and "of" end the type.
struct of_type_cfg {
    static constexpr auto term = dsl::token(dsl::ascii::space + kw_of) | end_of_part;
};

// ---------------------------------------------------------------- declarations
// (let|const) name (: type)?
template <typename TypeCfg>
struct var_head {
    static constexpr auto rule =
        dsl::position + dsl::capture(let_or_const) + dsl::p<name_p> +
        dsl::opt(dsl::lit_c<':'> >> dsl::p<scan_type<TypeCfg>>);
    static constexpr auto value = lexy::bind(
        lexy::callback<VarHead>([](const ParseState& st, const char* pos, const auto& kw,
                                   std::string name, auto type) {
            VarHead h;
            h.pos = st.offset(pos);
            h.is_const = kw.end() - kw.begin() == 5;
            h.name = std::move(name);
            if constexpr (!std::is_same_v<decltype(type), lexy::nullopt>) h.type = type_of(st, type);
            return h;
        }),
        lexy::parse_state, lexy::values);
};

// A declaration without the ";".
template <typename TypeCfg, typename InitCfg>
struct decl_p {
    static constexpr auto rule =
        dsl::p<var_head<TypeCfg>> + dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<InitCfg>>);
    static constexpr auto value = lexy::callback<DeclParts>(
        [](VarHead&& h, lexy::nullopt) { return DeclParts{std::move(h), false, {}}; },
        [](VarHead&& h, Span init) { return DeclParts{std::move(h), true, init}; });
};

constexpr auto semi = dsl::try_(dsl::lit_c<';'>);

struct stmt;

struct decl_stmt {
    static constexpr auto rule = dsl::p<decl_p<decl_type_cfg, init_cfg>> + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, DeclParts&& d) { return make_decl(st, std::move(d)); }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- simple statements
struct expr_stmt {
    static constexpr auto rule = dsl::position + dsl::p<scan_until<expr_cfg>> + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, Span sp) -> StmtP {
            ExprP e = expr_of(st, sp);
            if (!e) return nullptr;
            return ast::make_stmt(ast::ExprStmt{std::move(e), st.offset(pos)});
        }),
        lexy::parse_state, lexy::values);
};

struct return_stmt {
    static constexpr auto rule =
        dsl::position + kw_return + dsl::p<scan_until<expr_cfg>> + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, Span sp) -> StmtP {
            ast::ReturnStmt r;
            r.pos = st.offset(pos);
            if (!blank(st, sp)) {
                r.expr = expr_of(st, sp);
                if (!r.expr) return nullptr;
            }
            return ast::make_stmt(std::move(r));
        }),
        lexy::parse_state, lexy::values);
};

struct throw_stmt {
    static constexpr auto rule = dsl::position + kw_throw + dsl::p<scan_until<expr_cfg>> + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, Span sp) -> StmtP {
            ast::ThrowStmt t;
            t.pos = st.offset(pos);
            if (!blank(st, sp)) {
                t.expr = expr_of(st, sp);
                if (!t.expr) return nullptr;
            }
            return ast::make_stmt(std::move(t));
        }),
        lexy::parse_state, lexy::values);
};

struct break_stmt {
    static constexpr auto rule = dsl::position + kw_break + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos) {
            return ast::make_stmt(ast::BreakStmt{st.offset(pos)});
        }),
        lexy::parse_state, lexy::values);
};

struct continue_stmt {
    static constexpr auto rule = dsl::position + kw_continue + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos) {
            return ast::make_stmt(ast::ContinueStmt{st.offset(pos)});
        }),
        lexy::parse_state, lexy::values);
};

struct empty_stmt {
    static constexpr auto rule = dsl::position + dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos) {
            return ast::make_stmt(ast::EmptyStmt{st.offset(pos)});
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- recovery
// The end of the list of statements. The top list ends at the end of the text. A "}" in it
// is a stray char. A list in a block ends before the "}".
struct top_cfg {
    static constexpr auto closer = dsl::eof;
    static constexpr auto stop = dsl::lit_c<'}'> >> dsl::break_;
};
struct inner_cfg {
    static constexpr auto closer = dsl::eof | dsl::lit_c<'}'>;
    static constexpr auto stop = dsl::peek(dsl::lit_c<'}'>) >> dsl::break_;
};

// Skips the rest of a broken statement. It stops after a ";" and at the end of the text.
// It stops before the "}" of a block. A nested block, a string and a comment are skipped.
// A stray "}" in the top list is skipped, and the sync stops after it.
template <typename Cfg, typename T>
struct sync : lexy::token_production {
    static constexpr auto rule = dsl::loop(dsl::eof >> dsl::break_ | dsl::lit_c<';'> >> dsl::break_ |
                                           Cfg::stop | dsl::else_ >> dsl::p<skip_item>);
    static constexpr auto value = lexy::callback<T>([]() { return T{}; });
};

// ---------------------------------------------------------------- blocks
// The statements of a block: a list with nulls for the broken statements.
template <typename Cfg>
struct stmts_p {
    static constexpr auto item =
        dsl::peek_not(Cfg::closer) >>
        dsl::try_(dsl::recurse<stmt>, dsl::p<sync<Cfg, StmtP>>);
    static constexpr auto rule = dsl::opt(dsl::peek_not(Cfg::closer) >> dsl::list(item));
    static constexpr auto value = lexy::as_list<std::vector<StmtP>>;
};

struct block_p {
    static constexpr auto rule = dsl::position + dsl::lit_c<'{'> + dsl::p<stmts_p<inner_cfg>> +
                                 dsl::position + dsl::lit_c<'}'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* b, std::vector<StmtP>&& v,
                                 const char* e) {
            ast::BlockStmt blk;
            blk.stmts = keep(std::move(v));
            blk.pos = st.offset(b);
            blk.end_pos = st.offset(e);
            return ast::make_stmt(std::move(blk));
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- if, while, do
struct else_part {
    static constexpr auto rule = dsl::position + kw_else + dsl::recurse<stmt>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ElsePart>([](const ParseState& st, const char* pos, StmtP&& s) {
            return ElsePart{st.offset(pos), std::move(s)};
        }),
        lexy::parse_state, lexy::values);
};

struct if_stmt {
    static constexpr auto rule = dsl::position + kw_if + dsl::p<scan_until<cond_cfg>> +
                                 dsl::lit_c<')'> + dsl::recurse<stmt> +
                                 dsl::opt(dsl::peek(kw_else) >> dsl::p<else_part>);
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, Span cond, StmtP&& then,
                                 auto&& other) -> StmtP {
            ast::IfStmt n;
            n.pos = st.offset(pos);
            n.cond = expr_of(st, cond);
            n.then = std::move(then);
            if constexpr (!std::is_same_v<std::decay_t<decltype(other)>, lexy::nullopt>) {
                n.else_pos = other.pos;
                n.otherwise = std::move(other.stmt);
                if (!n.otherwise) return nullptr;
            }
            if (!n.cond || !n.then) return nullptr;
            return ast::make_stmt(std::move(n));
        }),
        lexy::parse_state, lexy::values);
};

struct while_stmt {
    static constexpr auto rule = dsl::position + kw_while + dsl::p<scan_until<cond_cfg>> +
                                 dsl::lit_c<')'> + dsl::recurse<stmt>;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, Span cond,
                                 StmtP&& body) -> StmtP {
            ast::WhileStmt n;
            n.pos = st.offset(pos);
            n.cond = expr_of(st, cond);
            n.body = std::move(body);
            if (!n.cond || !n.body) return nullptr;
            return ast::make_stmt(std::move(n));
        }),
        lexy::parse_state, lexy::values);
};

struct do_stmt {
    static constexpr auto rule = dsl::position + kw_do + dsl::recurse<stmt> + dsl::position +
                                 dsl::must(kw_while).template error<e_while> +
                                 dsl::p<scan_until<cond_cfg>> + dsl::lit_c<')'> + semi;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, StmtP&& body,
                                 const char* wpos, Span cond) -> StmtP {
            ast::DoWhileStmt n;
            n.pos = st.offset(pos);
            n.while_pos = st.offset(wpos);
            n.body = std::move(body);
            n.cond = expr_of(st, cond);
            if (!n.cond || !n.body) return nullptr;
            return ast::make_stmt(std::move(n));
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- try
// The type of a catch clause ends at the ")".
struct catch_type_cfg {
    static constexpr auto term = end_of_part;
};

// "...": the clause catches all.
struct catch_all {
    static constexpr auto rule = LEXY_LIT("...");
    static constexpr auto value = lexy::callback<ast::CatchClause>([]() {
        ast::CatchClause c;
        c.is_all = true;
        return c;
    });
};

// "name: type". A clause without a ":" has no name. Its name stays empty and the error is
// in the diagnostics.
struct catch_named {
    static constexpr auto rule = dsl::position + dsl::p<name_p> +
                                 dsl::opt(dsl::lit_c<':'> >> dsl::p<scan_type<catch_type_cfg>>);
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::CatchClause>(
            [](const ParseState& st, const char* pos, std::string&& name, auto type) {
                ast::CatchClause c;
                if constexpr (std::is_same_v<decltype(type), lexy::nullopt>) {
                    st.error(pos, "expected a name");
                } else if (blank(st, type)) {
                    st.error(st.base + type.begin, "expected a type");
                } else {
                    c.name = std::move(name);
                    c.type = type_of(st, type);
                }
                return c;
            }),
        lexy::parse_state, lexy::values);
};

struct catch_clause {
    static constexpr auto rule =
        dsl::position + kw_catch + dsl::lit_c<'('> +
        (dsl::peek(LEXY_LIT("...")) >> dsl::p<catch_all> | dsl::else_ >> dsl::p<catch_named>) +
        dsl::lit_c<')'> + dsl::p<block_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::CatchClause>(
            [](const ParseState& st, const char* pos, ast::CatchClause&& c, StmtP&& body) {
                c.pos = st.offset(pos);
                c.block = std::move(std::get<ast::BlockStmt>(body->v));
                return std::move(c);
            }),
        lexy::parse_state, lexy::values);
};

struct catch_list {
    static constexpr auto rule = dsl::list(dsl::peek(kw_catch) >> dsl::p<catch_clause>);
    static constexpr auto value = lexy::as_list<std::vector<ast::CatchClause>>;
};

struct try_stmt {
    static constexpr auto rule = dsl::position + kw_try + dsl::p<block_p> +
                                 dsl::must(dsl::peek(kw_catch)).template error<e_catch> +
                                 dsl::p<catch_list>;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, StmtP&& body,
                                 std::vector<ast::CatchClause>&& list) -> StmtP {
            ast::TryStmt n;
            n.pos = st.offset(pos);
            n.block = std::move(std::get<ast::BlockStmt>(body->v));
            for (const ast::CatchClause& c : list)
                if (!c.is_all && c.name.empty()) return nullptr;
            n.catches = std::move(list);
            return ast::make_stmt(std::move(n));
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- for
// "( (let|const) name (: type)? of" is the start of a for-of statement.
constexpr auto ws = dsl::whitespace(ws_rule);
// The look-ahead cannot recurse. So it skips the type flat, up to the white space before "of".
// A "=", a ";", a ")", a "}" or the end of the text before "of" means: not a for-of.
constexpr auto of_probe_type = dsl::loop(
    dsl::peek(dsl::ascii::space + kw_of) >> dsl::break_ |
    dsl::peek(dsl::eof | (dsl::lit_c<'='> / dsl::lit_c<';'> / dsl::lit_c<')'> / dsl::lit_c<'}'>)) >>
        dsl::error<e_stmt> |
    dsl::else_ >> any_unit);
constexpr auto for_of_head = dsl::lit_c<'('> + ws + let_or_const + ws + id + ws +
                             (kw_of | dsl::lit_c<':'> >> (of_probe_type + ws + kw_of));

struct for_of_rest {
    static constexpr auto rule = dsl::lit_c<'('> + dsl::p<var_head<of_type_cfg>> + kw_of +
                                 dsl::p<scan_until<part_cfg>> + dsl::lit_c<')'> +
                                 dsl::recurse<stmt>;
    static constexpr auto value = lexy::callback<ForOfRest>(
        [](VarHead&& h, Span range, StmtP&& body) {
            return ForOfRest{std::move(h), range, std::move(body)};
        });
};

struct for_init {
    static constexpr auto rule =
        dsl::peek(let_or_const) >> dsl::p<decl_p<for_type_cfg, for_init_cfg>> |
        dsl::else_ >> dsl::p<scan_until<part_cfg>>;
    static constexpr auto value = lexy::callback<ForInit>(
        [](DeclParts&& d) {
            ForInit i;
            i.is_decl = true;
            i.decl = std::move(d);
            return i;
        },
        [](Span sp) {
            ForInit i;
            i.span = sp;
            return i;
        });
};

struct for_classic_rest {
    static constexpr auto rule = dsl::lit_c<'('> + dsl::p<for_init> + dsl::lit_c<';'> +
                                 dsl::p<scan_until<part_cfg>> + dsl::lit_c<';'> +
                                 dsl::p<scan_until<part_cfg>> + dsl::lit_c<')'> +
                                 dsl::recurse<stmt>;
    static constexpr auto value = lexy::callback<ForRest>(
        [](ForInit&& i, Span cond, Span step, StmtP&& body) {
            return ForRest{std::move(i), cond, step, std::move(body)};
        });
};

struct for_stmt {
    static constexpr auto rule =
        dsl::position + kw_for +
        (dsl::peek(for_of_head) >> dsl::p<for_of_rest> | dsl::else_ >> dsl::p<for_classic_rest>);
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>(
            [](const ParseState& st, const char* pos, ForOfRest&& r) -> StmtP {
                ast::ForOfStmt n;
                n.pos = st.offset(pos);
                n.is_const = r.head.is_const;
                n.name = std::move(r.head.name);
                n.type = std::move(r.head.type);
                n.range = expr_of(st, r.range);
                n.body = std::move(r.body);
                if (!n.range || !n.body) return nullptr;
                return ast::make_stmt(std::move(n));
            },
            [](const ParseState& st, const char* pos, ForRest&& r) -> StmtP {
                ast::ForStmt n;
                n.pos = st.offset(pos);
                bool ok = true;
                if (r.init.is_decl) {
                    StmtP d = make_decl(st, std::move(r.init.decl));
                    if (d) n.init.push_back(std::move(d));
                    else ok = false;
                } else {
                    std::vector<ExprP> list;
                    if (list_of(st, r.init.span, list)) {
                        for (ExprP& e : list) {
                            const uint32_t p = ast::pos_of(*e);
                            n.init.push_back(ast::make_stmt(ast::ExprStmt{std::move(e), p}));
                        }
                    } else {
                        ok = false;
                    }
                }
                if (!blank(st, r.cond)) {
                    n.cond = expr_of(st, r.cond);
                    if (!n.cond) ok = false;
                }
                if (!list_of(st, r.step, n.step)) ok = false;
                n.body = std::move(r.body);
                if (!ok || !n.body) return nullptr;
                return ast::make_stmt(std::move(n));
            }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- switch
// The text of a case label. It ends before the first ":" at depth 0.
// A ";" and a "}" at depth 0 end it too.
struct case_span : lexy::token_production {
    static constexpr auto rule =
        dsl::position +
        dsl::loop(dsl::peek(dsl::eof | (dsl::lit_c<':'> / dsl::lit_c<';'> / dsl::lit_c<'}'>)) >>
                      dsl::break_ |
                  dsl::else_ >> dsl::p<until_item>) +
        dsl::position;
    static constexpr auto value = span_value;
};

struct case_label {
    static constexpr auto rule =
        dsl::position + kw_case + dsl::p<case_span> + dsl::lit_c<':'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<SwItem>([](const ParseState& st, const char* pos, Span label) {
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

struct default_label {
    static constexpr auto rule = dsl::position + kw_default + dsl::lit_c<':'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<SwItem>([](const ParseState& st, const char* pos) {
            SwItem i;
            i.kind = SwItem::Default;
            i.pos = st.offset(pos);
            return i;
        }),
        lexy::parse_state, lexy::values);
};

struct sw_item {
    static constexpr auto rule = dsl::peek(kw_case) >> dsl::p<case_label> |
                                 dsl::peek(kw_default) >> dsl::p<default_label> |
                                 dsl::else_ >> dsl::recurse<stmt>;
    static constexpr auto value = lexy::bind(
        lexy::callback<SwItem>(
            [](const ParseState&, SwItem&& i) { return std::move(i); },
            [](const ParseState&, StmtP&& s) {
                SwItem i;
                i.kind = SwItem::Statement;
                if (s) i.pos = ast::pos_of(*s);
                i.stmt = std::move(s);
                return i;
            }),
        lexy::parse_state, lexy::values);
};

struct sw_items {
    static constexpr auto item =
        dsl::peek_not(inner_cfg::closer) >>
        dsl::try_(dsl::p<sw_item>, dsl::p<sync<inner_cfg, SwItem>>);
    static constexpr auto rule = dsl::opt(dsl::peek_not(inner_cfg::closer) >> dsl::list(item));
    static constexpr auto value = lexy::as_list<std::vector<SwItem>>;
};

struct switch_stmt {
    static constexpr auto rule = dsl::position + kw_switch + dsl::p<scan_until<cond_cfg>> +
                                 dsl::lit_c<')'> + dsl::lit_c<'{'> + dsl::p<sw_items> +
                                 dsl::position + dsl::lit_c<'}'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<StmtP>([](const ParseState& st, const char* pos, Span subject,
                                 std::vector<SwItem>&& items, const char* end) -> StmtP {
            ast::SwitchStmt n;
            n.pos = st.offset(pos);
            n.end_pos = st.offset(end);
            n.subject = expr_of(st, subject);
            // A label after a statement, or a case after a default, starts a new group.
            bool open = false;
            for (SwItem& it : items) {
                if (it.kind == SwItem::None) continue;
                if (it.kind == SwItem::Statement) {
                    if (!it.stmt) continue;
                    if (!open) {
                        st.error(st.base + it.pos, "expected 'case' or 'default'");
                        continue;
                    }
                    n.cases.back().stmts.push_back(std::move(it.stmt));
                    continue;
                }
                const bool fresh = n.cases.empty() || !open || !n.cases.back().stmts.empty() ||
                                   n.cases.back().is_default;
                if (fresh) {
                    n.cases.emplace_back();
                    n.cases.back().pos = it.pos;
                }
                open = true;
                if (it.kind == SwItem::Default) {
                    n.cases.back().is_default = true;
                    n.cases.back().default_pos = it.pos;
                } else if (it.label) {
                    n.cases.back().labels.push_back(std::move(it.label));
                    n.cases.back().label_pos.push_back(it.pos);
                }
            }
            if (!n.subject) return nullptr;
            return ast::make_stmt(std::move(n));
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- statement
struct stmt {
    static constexpr auto rule =
        dsl::peek(dsl::lit_c<'{'>) >> dsl::p<block_p> |
        dsl::peek(let_or_const) >> dsl::p<decl_stmt> | dsl::peek(kw_if) >> dsl::p<if_stmt> |
        dsl::peek(kw_while) >> dsl::p<while_stmt> | dsl::peek(kw_do) >> dsl::p<do_stmt> |
        dsl::peek(kw_for) >> dsl::p<for_stmt> | dsl::peek(kw_return) >> dsl::p<return_stmt> |
        dsl::peek(kw_break) >> dsl::p<break_stmt> |
        dsl::peek(kw_continue) >> dsl::p<continue_stmt> |
        dsl::peek(kw_switch) >> dsl::p<switch_stmt> | dsl::peek(kw_throw) >> dsl::p<throw_stmt> |
        dsl::peek(kw_try) >> dsl::p<try_stmt> |
        dsl::peek(dsl::lit_c<';'>) >> dsl::p<empty_stmt> |
        dsl::peek(dsl::eof | dsl::lit_c<'}'>) >> dsl::error<e_stmt> |
        dsl::else_ >> dsl::p<expr_stmt>;
    static constexpr auto value = lexy::forward<StmtP>;
};

// ---------------------------------------------------------------- the entry point
struct block_entry {
    static constexpr auto whitespace = ws_rule;
    static constexpr auto max_recursion_depth = max_depth;
    static constexpr auto rule = dsl::whitespace(ws_rule) + dsl::p<stmts_p<top_cfg>>;
    static constexpr auto value = lexy::forward<std::vector<StmtP>>;
};

// Counts the nested calls of parse_block in this thread.
struct NestingGuard {
    static thread_local int depth;
    NestingGuard() { ++depth; }
    ~NestingGuard() { --depth; }
    NestingGuard(const NestingGuard&) = delete;
    NestingGuard& operator=(const NestingGuard&) = delete;
};
thread_local int NestingGuard::depth = 0;

}  // namespace

ast::BlockStmt parse_block(ParseInput in, Span body_without_braces, Diagnostics& diag)
{
    ast::BlockStmt block;
    block.pos = body_without_braces.begin > 0 ? body_without_braces.begin - 1 : 0;
    block.end_pos = body_without_braces.end;

    const std::size_t size = in.text.size();
    const std::size_t end = body_without_braces.end < size ? body_without_braces.end : size;
    const std::size_t begin = body_without_braces.begin < end ? body_without_braces.begin : end;

    NestingGuard guard;
    if (NestingGuard::depth > max_block_nesting) {
        diag.add(static_cast<uint32_t>(begin), *in.map, "maximum nesting depth exceeded");
        return block;
    }
    const size_t first_item = diag.items.size();
    const ParseState st{in.text.data(), in.map, &diag, size};
    const auto input = lexy::string_input<Encoding>(in.text.data() + begin, end - begin);
    auto result = parse_with_diag<block_entry>(input, st);
    // The parse reports the recursion limit again at each column. Keep the first error and
    // drop the errors after it: the rest of the body is not parsed.
    for (size_t i = first_item; i < diag.items.size(); ++i) {
        if (diag.items[i].msg == "maximum recursion depth exceeded") {
            diag.items.resize(i + 1);
            break;
        }
    }
    if (result.has_value()) block.stmts = keep(std::move(result).value());
    return block;
}

}  // namespace spc
