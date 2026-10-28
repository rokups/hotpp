// The grammar of the script structure: imports, classes, fields and methods.
//
// The grammar finds the end of each method body and of each field initializer and stores the
// span. After the parse, parse_script calls parse_block and parse_expression on each span.
// So one call gives the errors of the whole file. The function sorts them by position.
// Error recovery works at two levels. A broken member skips to the next ";" or "}" or to the next member. A broken
// import or class skips to the next "import" or "class".
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "expression.h"
#include "lexy_common.hpp"
#include "statement.h"

namespace spc {
namespace {

using namespace parser;  // NOLINT: the grammar uses the names of lexy_common.hpp

// ---------------------------------------------------------------- error texts
SPC_ERROR_TAG(e_top, "expected 'import' or 'class'");
SPC_ERROR_TAG(e_member, "expected a field, '[' or 'function'");
SPC_ERROR_TAG(e_class_name, "expected a class name");
SPC_ERROR_TAG(e_base_name, "expected a base class name");
SPC_ERROR_TAG(e_method_name, "expected a method name");
SPC_ERROR_TAG(e_field_name, "expected a field name");
SPC_ERROR_TAG(e_param_name, "expected a parameter name");
SPC_ERROR_TAG(e_attr_name, "expected an attribute name");
SPC_ERROR_TAG(e_alias_string, "expected a string in [alias(...)]");
SPC_ERROR_TAG(e_import_name, "import needs a header name");
SPC_ERROR_TAG(e_import_arg, "import needs a string or <header>");
SPC_ERROR_TAG(e_import_close, "import <...> needs a closing '>'");

// The text of a span.
std::string_view span_text(const ParseState& st, Span sp)
{
    return std::string_view(st.base + sp.begin, sp.end - sp.begin);
}

// The type text of a span. A "." is a scope and becomes "::". A "::" is an error.
std::string type_of(const ParseState& st, Span sp) { return type_of_span(st, sp); }

// ---------------------------------------------------------------- values
// The attributes before a field. The value is the alias, or empty.
struct Annot {
    const char* pos = nullptr;  // the "[" or the "@"
    std::string alias;
    bool has_alias = false;
};

// One attribute: a name and a string.
struct AttrItem {
    const char* pos = nullptr;  // the name
    std::string name;
    std::string arg;
};

struct MethodRest {
    std::string name;
    std::vector<ast::Param> params;
    std::string ret;
    Span body;
};

struct Member {
    enum Kind { None, IsMethod, IsField } kind = None;
    ast::Method method;
    ast::Field field;
};

struct TopItem {
    enum Kind { None, IsImport, IsClass } kind = None;
    ast::Import import;
    ast::ClassDef cls;
};

const Annot* annot_of(lexy::nullopt) { return nullptr; }
const Annot* annot_of(const Annot& a) { return &a; }

// ---------------------------------------------------------------- tokens
// An identifier. The Tag is the error text when there is none.
template <typename Tag>
struct name_p : lexy::token_production {
    static constexpr auto rule = dsl::must(id).template error<Tag>;
    static constexpr auto value = lexy::as_string<std::string>;
};

// A string literal. The value is the raw text between the quotes.
struct string_value : lexy::token_production {
    static constexpr auto rule = string_lit;
    static constexpr auto value = lexy::callback<std::string>(
        [](const char* b, const char* e) { return std::string(b, e); });
};

// "<" ... ">" on one line. The value keeps the angle brackets and drops the white space.
struct angle_value : lexy::token_production {
    static constexpr auto rule =
        dsl::lit_c<'<'> >>
        (dsl::position +
            dsl::loop(dsl::peek(dsl::lit_c<'>'>) >> dsl::break_ |
                      dsl::peek(dsl::newline) >> dsl::error<e_import_close> |
                      dsl::eof >> dsl::error<e_import_close> | dsl::else_ >> any_unit) +
            dsl::position + dsl::lit_c<'>'>);
    static constexpr auto value = lexy::callback<std::string>([](const char* b, const char* e) {
        std::string path = "<";
        for (; b != e; ++b)
            if (!std::isspace(static_cast<unsigned char>(*b))) path += *b;
        return path + ">";
    });
};

// ---------------------------------------------------------------- types
struct param_term {
    static constexpr auto term = dsl::lit_c<','> / dsl::lit_c<')'>;
};
struct ret_term {
    static constexpr auto term = dsl::lit_c<'{'>;
};
struct field_term {
    static constexpr auto term = dsl::lit_c<'='> / dsl::lit_c<';'> / dsl::lit_c<'}'>;
};
struct init_cfg {
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term =
        dsl::lit_c<';'> / dsl::lit_c<')'> / dsl::lit_c<']'> / dsl::lit_c<'}'>;
};

// ---------------------------------------------------------------- imports
struct import_p {
    static constexpr auto rule =
        dsl::position + kw_import +
        (dsl::eof >> dsl::error<e_import_name> |
         dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_value> |
         dsl::peek(dsl::lit_c<'<'>) >> dsl::p<angle_value> |
         dsl::else_ >> dsl::error<e_import_arg>) +
        dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Import>([](const ParseState& st, const char* pos, std::string path) {
            if (path.size() > 2 && path.front() == '<' && path.back() != '>')
                st.error(pos, "import <...> needs a closing '>'");
            else if (path == "<>" || path.empty())
                st.error(pos, "import needs a header name");
            ast::Import im;
            im.path = std::move(path);
            im.pos = st.offset(pos);
            im.line = st.map->line(im.pos);
            return im;
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- members
// One attribute in the list: name "(" string ")".
struct attr_p {
    static constexpr auto rule =
        dsl::position + dsl::p<name_p<e_attr_name>> + dsl::lit_c<'('> +
        dsl::must(dsl::peek(dsl::lit_c<'"'>)).template error<e_alias_string> +
        dsl::p<string_value> + dsl::lit_c<')'>;
    static constexpr auto value = lexy::callback<AttrItem>(
        [](const char* pos, std::string name, std::string arg) {
            return AttrItem{pos, std::move(name), std::move(arg)};
        });
};

struct attr_list {
    static constexpr auto rule = dsl::terminator(dsl::lit_c<']'>).list(
        dsl::p<attr_p>, dsl::sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<AttrItem>>;
};

// "[" attribute { "," attribute } "]". Only "alias" is known.
struct attrs_p {
    static constexpr auto rule = dsl::position + dsl::lit_c<'['> + dsl::p<attr_list>;
    static constexpr auto value = lexy::bind(
        lexy::callback<Annot>([](const ParseState& st, const char* pos,
                                 std::vector<AttrItem>&& items) {
            Annot a;
            a.pos = pos;
            for (AttrItem& it : items) {
                if (it.name != "alias")
                    st.error(it.pos, "unknown attribute '" + it.name + "'");
                else if (a.has_alias)
                    st.error(it.pos, "duplicate attribute 'alias'");
                else {
                    a.has_alias = true;
                    a.alias = std::move(it.arg);
                }
            }
            return a;
        }),
        lexy::parse_state, lexy::values);
};

struct paren_cfg {
    static constexpr auto open = dsl::lit_c<'('>;
    static constexpr auto term = dsl::lit_c<')'>;
};

// The removed form "@name(...)". The parser reports it and goes on with the member.
struct old_attr_p {
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'@'> + dsl::p<name_p<e_attr_name>> +
        dsl::opt(dsl::peek(dsl::lit_c<'('>) >> dsl::p<scan_until<paren_cfg>> + dsl::lit_c<')'>);
    static constexpr auto value = lexy::bind(
        lexy::callback<Annot>([](const ParseState& st, const char* pos, std::string, auto&&) {
            st.error(pos, "use [alias(...)] instead of @was(...)");
            Annot a;
            a.pos = pos;
            return a;
        }),
        lexy::parse_state, lexy::values);
};

struct annot_p {
    static constexpr auto rule =
        dsl::peek(dsl::lit_c<'@'>) >> dsl::p<old_attr_p> | dsl::else_ >> dsl::p<attrs_p>;
    static constexpr auto value = lexy::forward<Annot>;
};

struct param_p {
    static constexpr auto rule = dsl::p<name_p<e_param_name>> + dsl::lit_c<':'> +
                                 dsl::p<scan_type<param_term>> + dsl::if_(dsl::lit_c<','>);
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Param>([](const ParseState& st, std::string name, Span type) {
            return ast::Param{std::move(name), type_of(st, type)};
        }),
        lexy::parse_state, lexy::values);
};

struct params_p {
    static constexpr auto rule =
        dsl::lit_c<'('> + dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::p<param_p>);
    static constexpr auto value = lexy::as_list<std::vector<ast::Param>>;
};

// The part of a method after the keyword: name, params, return type and body.
struct method_rest {
    static constexpr auto rule =
        dsl::p<name_p<e_method_name>> + dsl::p<params_p> +
        dsl::opt(LEXY_LIT("->") >> dsl::p<scan_type<ret_term>>) + dsl::p<scan_balanced>;
    static constexpr auto value = lexy::bind(
        lexy::callback<MethodRest>([](const ParseState& st, std::string name, auto&& params,
                                      auto&& ret, Span body) {
            MethodRest r;
            r.name = std::move(name);
            if constexpr (!std::is_same_v<std::decay_t<decltype(params)>, lexy::nullopt>)
                r.params = std::move(params);
            if constexpr (!std::is_same_v<std::decay_t<decltype(ret)>, lexy::nullopt>)
                r.ret = type_of(st, ret);
            r.body = body;
            return r;
        }),
        lexy::parse_state, lexy::values);
};

struct field_rest {
    static constexpr auto rule =
        dsl::position + dsl::p<name_p<e_field_name>> + dsl::lit_c<':'> +
        dsl::p<scan_type<field_term>> +
        dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<init_cfg>>) + dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Field>(
            [](const ParseState& st, const char* pos, std::string name, Span type, auto init) {
                ast::Field f;
                f.name = std::move(name);
                f.type = type_of(st, type);
                if constexpr (!std::is_same_v<decltype(init), lexy::nullopt>) {
                    f.has_init = true;
                    f.init_span = init;
                    if (only_blank(span_text(st, init)))
                        st.error(st.base + init.begin, "expected an initializer");
                }
                f.pos = st.offset(pos);
                f.line = st.map->line(f.pos);
                return f;
            }),
        lexy::parse_state, lexy::values);
};

// The junk token for the error text: a word or one code point.
constexpr auto junk = dsl::while_one(dsl::ascii::word) | dsl::else_ >> any_unit;

struct member_p {
    static constexpr auto rule =
        dsl::position +
        dsl::opt(dsl::peek(dsl::lit_c<'@'> / dsl::lit_c<'['>) >> dsl::p<annot_p>) +
        (dsl::eof >> dsl::error<e_unexpected_eof> |
         dsl::capture(kw_function) >> dsl::p<method_rest> |
         // "override" gives one error. A "function" after it is skipped. The parse goes on.
         dsl::capture(kw_override) >> dsl::if_(kw_function) + dsl::p<method_rest> |
         dsl::peek(id + dsl::whitespace(ws_rule) + dsl::lit_c<':'> +
                   dsl::peek_not(dsl::lit_c<':'>)) >>
             dsl::p<field_rest> |
         dsl::else_ >> dsl::error<e_member>(junk));
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>(
            [](const ParseState& st, const char*, const auto& ann, const auto& kw,
               MethodRest&& rest) {
                Member m;
                m.kind = Member::IsMethod;
                ast::Method& me = m.method;
                if (std::string_view(kw.begin(), kw.size()) == "override")
                    st.error(kw.begin(),
                             "'override' is removed; a function overrides a virtual method implicitly");
                me.name = std::move(rest.name);
                me.params = std::move(rest.params);
                me.ret = std::move(rest.ret);
                me.body_span = rest.body;
                me.pos = st.offset(kw.begin());
                me.line = st.map->line(me.pos);
                me.end_pos = rest.body.end;
                me.end_line = st.map->line(me.end_pos);
                if (const Annot* a = annot_of(ann))
                    if (a->has_alias) st.error(a->pos, "[alias] applies to fields only");
                return m;
            },
            [](const ParseState&, const char*, const auto& ann, ast::Field&& f) {
                Member m;
                m.kind = Member::IsField;
                m.field = std::move(f);
                if (const Annot* a = annot_of(ann)) m.field.alias = a->alias;
                return m;
            }),
        lexy::parse_state, lexy::values);
};

// Skips the rest of a broken member. It stops after a ";" or before a "}", "override" or "function".
struct sync_member : lexy::token_production {
    static constexpr auto rule = dsl::loop(
        dsl::eof >> dsl::error<e_unexpected_eof> | dsl::lit_c<';'> >> dsl::break_ |
        dsl::peek(dsl::literal_set(dsl::lit_c<'}'>, kw_override, kw_function)) >> dsl::break_ |
        dsl::else_ >> dsl::p<skip_item>);
    static constexpr auto value = lexy::noop;
};

struct members_p {
    static constexpr auto rule = dsl::terminator(dsl::lit_c<'}'>).opt_list(
        dsl::try_(dsl::p<member_p>, dsl::p<sync_member>));
    static constexpr auto value = lexy::as_list<std::vector<Member>>;
};

// ---------------------------------------------------------------- classes
// A base class name with optional "." parts. The text joins the parts with "::".
// A "::" in the name is an error.
struct base_p {
    static constexpr auto rule =
        dsl::if_(dsl::peek(LEXY_LIT("::")) >> dsl::error<e_use_dot_colons>) +
        dsl::list(dsl::p<name_p<e_base_name>>, dsl::sep(dsl::lit_c<'.'>)) +
        dsl::if_(dsl::peek(LEXY_LIT("::")) >> dsl::error<e_use_dot_colons>);
    static constexpr auto value = lexy::fold_inplace<std::string>(
        std::string{}, [](std::string& acc, std::string&& part) {
            if (!acc.empty()) acc += "::";
            acc += part;
        });
};

struct class_p {
    static constexpr auto rule = dsl::position + kw_class + dsl::p<name_p<e_class_name>> +
                                 dsl::lit_c<':'> + dsl::p<base_p> + dsl::lit_c<'{'> +
                                 dsl::p<members_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::ClassDef>([](const ParseState& st, const char* pos, std::string name,
                                         std::string base, lexy::nullopt) {
            ast::ClassDef c;
            c.name = std::move(name);
            c.base = std::move(base);
            c.pos = st.offset(pos);
            c.line = st.map->line(c.pos);
            return c;
        },
        [](const ParseState& st, const char* pos, std::string name, std::string base,
           std::vector<Member>&& members) {
            ast::ClassDef c;
            c.name = std::move(name);
            c.base = std::move(base);
            c.pos = st.offset(pos);
            c.line = st.map->line(c.pos);
            for (Member& m : members) {
                if (m.kind == Member::IsMethod)
                    c.methods.push_back(std::move(m.method));
                else if (m.kind == Member::IsField)
                    c.fields.push_back(std::move(m.field));
            }
            return c;
        }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- file
struct decl {
    static constexpr auto rule =
        dsl::peek(kw_import) >> dsl::p<import_p> | dsl::peek(kw_class) >> dsl::p<class_p> |
        dsl::else_ >> dsl::error<e_top>(junk);
    static constexpr auto value = lexy::callback<TopItem>(
        [](ast::Import&& im) {
            TopItem t;
            t.kind = TopItem::IsImport;
            t.import = std::move(im);
            return t;
        },
        [](ast::ClassDef&& c) {
            TopItem t;
            t.kind = TopItem::IsClass;
            t.cls = std::move(c);
            return t;
        });
};

// Skips a broken declaration. It stops before the next "import" or "class".
struct sync_decl : lexy::token_production {
    static constexpr auto rule = dsl::loop(
        dsl::eof >> dsl::break_ |  // the end of the file is not a new error after a broken decl
        dsl::peek(dsl::literal_set(kw_import, kw_class)) >> dsl::break_ |
        dsl::else_ >> dsl::p<skip_item>);
    static constexpr auto value = lexy::noop;
};

struct file_p {
    static constexpr auto whitespace = ws_rule;
    static constexpr auto rule =
        dsl::terminator(dsl::eof).opt_list(dsl::try_(dsl::p<decl>, dsl::p<sync_decl>));
    static constexpr auto value = lexy::as_list<std::vector<TopItem>>;
};

}  // namespace

ast::Script parse_script(std::string_view text, const LineMap& map, Diagnostics& diag)
{
    ast::Script script;
    const size_t first_item = diag.items.size();
    const ParseState st{text.data(), &map, &diag, text.size()};
    const auto input = lexy::string_input<Encoding>(text.data(), text.size());
    auto result = parse_with_diag<file_p>(input, st);
    if (result.has_value()) {
        for (TopItem& t : std::move(result).value()) {
            if (t.kind == TopItem::IsImport)
                script.imports.push_back(std::move(t.import));
            else if (t.kind == TopItem::IsClass)
                script.classes.push_back(std::move(t.cls));
        }
    }
    // The span-then-call step: parse each body and each initializer.
    const ParseInput in{text, &map};
    for (ast::ClassDef& c : script.classes) {
        for (ast::Method& m : c.methods) m.body = parse_block(in, m.body_span, diag);
        for (ast::Field& f : c.fields)
            if (f.has_init && !only_blank(span_text(st, f.init_span)))
                f.init = parse_expression(in, f.init_span, diag);
    }
    // An unterminated comment or literal reads to the end of the file. The "unexpected end of
    // file" errors from this place on are not new faults. Drop them.
    uint32_t cut = UINT32_MAX;
    for (size_t i = first_item; i < diag.items.size(); ++i)
        if (diag.items[i].msg.compare(0, 12, "unterminated") == 0)
            cut = std::min(cut, diag.items[i].pos);
    diag.items.erase(std::remove_if(diag.items.begin() + static_cast<std::ptrdiff_t>(first_item),
                                    diag.items.end(),
                                    [&](const Diag& d) {
                                        return d.msg == "unexpected end of file" && d.pos >= cut;
                                    }),
                     diag.items.end());
    // The errors of the structure come before the errors of the bodies. Sort them.
    std::stable_sort(diag.items.begin() + static_cast<std::ptrdiff_t>(first_item), diag.items.end(),
                     [](const Diag& a, const Diag& b) { return a.pos < b.pos; });
    return script;
}

}  // namespace spc
