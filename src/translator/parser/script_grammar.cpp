// The grammar of the script structure: imports, classes, fields and methods.
//
// The grammar finds the end of each method body and of each field initializer and stores the
// span. After the parse, parse_script calls parse_block and parse_expression on each span.
// So one call gives the errors of the whole file. The function sorts them by position.
// Error recovery works at two levels. A broken member skips to the next ";" or "}" or to the next member. A broken
// using declaration or class skips to the next "using" or "class".
#include "parser.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "expression.h"
#include "lexy_common.hpp"
#include "statement.h"

namespace sppc {
namespace {

using namespace parser;  // NOLINT: the grammar uses the names of lexy_common.hpp

// ---------------------------------------------------------------- error texts
SPP_ERROR_TAG(e_top, "expected 'using', 'class', 'struct', 'enum' or a variable");
SPP_ERROR_TAG(e_enum_name, "expected an enum name");
SPP_ERROR_TAG(e_enumerator, "expected an enumerator name");
SPP_ERROR_TAG(e_member, "expected a member: 'Type name;' or 'Ret name(...)'");
SPP_ERROR_TAG(e_member_name, "expected a member name");
SPP_ERROR_TAG(e_old_field, "field syntax changed; write 'Type name;'");
SPP_ERROR_TAG(e_class_name, "expected a class name");
SPP_ERROR_TAG(e_base_name, "expected a base class name");
SPP_ERROR_TAG(e_method_name, "expected a method name");
SPP_ERROR_TAG(e_field_name, "expected a field name");
SPP_ERROR_TAG(e_param_name, "expected a parameter name");
SPP_ERROR_TAG(e_old_param, "parameter syntax changed; write 'Type name'");
SPP_ERROR_TAG(e_attr_name, "expected an attribute name");
SPP_ERROR_TAG(e_alias_string, "expected a string in [alias(...)]");
SPP_ERROR_TAG(e_import_name, "using needs a header name");
SPP_ERROR_TAG(e_import_arg, "using needs a string or <header>");
SPP_ERROR_TAG(e_import_close, "using <...> needs a closing '>'");
SPP_ERROR_TAG(e_accessor, "expected 'get' or 'set'");
SPP_ERROR_TAG(e_super, "expected 'super(...)' after ':' of a constructor");

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

// One attribute: a name and a list of strings. The list can be empty.
struct AttrItem {
    const char* pos = nullptr;  // the name
    std::string name;
    std::vector<std::string> args;
};

// The base call of a constructor: ": super(args)".
struct SuperCall {
    const char* pos = nullptr;  // the "super" word
    Span args;                  // the text between the parentheses
};

struct MethodRest {
    std::string name;
    std::vector<ast::Param> params;
    std::string ret;
    const char* arrow = nullptr;  // the removed "->", or null
    const char* super_pos = nullptr;  // the "super" word of a base call, or null
    Span super_args;
    Span body;
};

// The part of a field after the name.
struct FieldRest {
    bool has_init = false;
    Span init;
};

// One accessor of a property: an optional access word, "get" or "set", and ";" or a body.
struct AccessorItem {
    const char* pos = nullptr;  // the "get" or "set" word
    bool is_get = false;
    bool has_body = false;
    Span body;
    bool has_access = false;
    ast::Access access = ast::Access::Public;
};

// The part of a property after the name.
struct PropRest {
    const char* open = nullptr;  // the "{"
    std::vector<AccessorItem> accessors;
    bool has_init = false;
    Span init;
};

struct Member {
    enum Kind { None, IsMethod, IsField, IsProp, IsCtor, IsDtor } kind = None;
    ast::Method method;
    ast::Ctor ctor;          // a member with no type: the add step checks the name
    std::string ctor_name;
    ast::Field field;
    ast::Property prop;
    bool get_has_access = false, set_has_access = false;  // an access word on the accessor
};

// The words "get", "set", "ref" and "out" are reserved. They are not names of members or parameters.
void check_name(const ParseState& st, const char* pos, const std::string& name)
{
    if (name == "get" || name == "set" || name == "ref" || name == "out")
        st.error(pos, "'" + name + "' is a reserved word");
}

struct TopItem {
    enum Kind { None, IsImport, IsClass, IsStub, IsEnum, IsVar } kind = None;
    ast::Import import;
    ast::ClassDef cls;
    ast::Stub stub;
    ast::EnumDef en;
    ast::Field var;  // a module variable
};

const Annot* annot_of(lexy::nullopt) { return nullptr; }
const Annot* annot_of(const Annot& a) { return &a; }

// ---------------------------------------------------------------- modifiers
// One modifier word and its position.
struct ModTok {
    const char* pos = nullptr;
    std::string word;
};

// The modifiers before a member. The parser checks the rules that do not depend on the kind.
struct Mods {
    bool is_override = false, is_virtual = false, is_new = false;
    bool has_access = false;
    ast::Access access = ast::Access::Public;
    std::vector<ModTok> kind_toks;  // the first "override", "virtual" or "new" words
};

bool is_access_word(const std::string& w)
{
    return w == "public" || w == "private" || w == "protected";
}

// Builds the Mods from the word list. It reports duplicates and words that cannot combine.
Mods collect_mods(const ParseState& st, const std::vector<ModTok>& toks)
{
    Mods m;
    std::vector<const ModTok*> seen;
    const ModTok* access_tok = nullptr;
    const ModTok* kind_tok = nullptr;
    for (const ModTok& t : toks) {
        bool dup = false;
        for (const ModTok* o : seen) dup = dup || o->word == t.word;
        if (dup) {
            st.error(t.pos, "duplicate modifier '" + t.word + "'");
            continue;
        }
        seen.push_back(&t);
        if (is_access_word(t.word)) {
            if (access_tok) {
                st.error(t.pos, "'" + access_tok->word + "' and '" + t.word + "' cannot combine");
                continue;
            }
            access_tok = &t;
            m.has_access = true;
            m.access = t.word == "public"    ? ast::Access::Public
                       : t.word == "private" ? ast::Access::Private
                                             : ast::Access::Protected;
        } else {
            if (kind_tok) {
                st.error(t.pos, "'" + kind_tok->word + "' and '" + t.word + "' cannot combine");
                continue;
            }
            kind_tok = &t;
            m.kind_toks.push_back(t);
            m.is_override = t.word == "override";
            m.is_virtual = t.word == "virtual";
            m.is_new = t.word == "new";
        }
    }
    return m;
}

Mods mods_of(const ParseState&, lexy::nullopt) { return Mods{}; }
Mods mods_of(const ParseState& st, const std::vector<ModTok>& v) { return collect_mods(st, v); }

// Applies the attributes and modifiers before a member to the member.
template <typename Ann, typename ModsT>
Member finish_member(const ParseState& st, const Ann& ann, const ModsT& mods_in, Member&& m)
{
    if (const Annot* a = annot_of(ann)) {
        if (m.kind == Member::IsField)
            m.field.alias = a->alias;
        else if (a->has_alias)
            st.error(a->pos, "[alias] applies to fields only");
    }
    const Mods mods = mods_of(st, mods_in);
    if (m.kind == Member::IsDtor) {
        m.method.access = mods.access;
        for (const ModTok& t : mods.kind_toks)
            st.error(t.pos, "'" + t.word + "' is not valid on a destructor");
    } else if (m.kind == Member::IsCtor) {
        m.ctor.access = mods.access;
        for (const ModTok& t : mods.kind_toks)
            st.error(t.pos, "'" + t.word + "' is not valid on a constructor");
    } else if (m.kind == Member::IsField || m.kind == Member::IsProp) {
        if (m.kind == Member::IsField) {
            m.field.access = mods.access;
        } else {
            // An accessor without its own access word has the access of the property.
            m.prop.access = mods.access;
            if (!m.get_has_access) m.prop.get_access = mods.access;
            if (!m.set_has_access) m.prop.set_access = mods.access;
        }
        for (const ModTok& t : mods.kind_toks)
            st.error(t.pos, "'" + t.word + "' is only valid on a method");
    } else {
        m.method.access = mods.access;
        m.method.is_override = mods.is_override;
        m.method.is_virtual = mods.is_virtual;
        m.method.is_new = mods.is_new;
    }
    return std::move(m);
}

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
// The removed word "import" has the same form as "using". The parser reports it and goes on.
template <bool Removed>
constexpr auto import_kw()
{
    if constexpr (Removed)
        return kw_import;
    else
        return kw_using;
}

template <bool Removed>
struct import_t {
    static constexpr auto rule =
        dsl::position + import_kw<Removed>() +
        (dsl::eof >> dsl::error<e_import_name> |
         dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_value> |
         dsl::peek(dsl::lit_c<'<'>) >> dsl::p<angle_value> |
         dsl::else_ >> dsl::error<e_import_arg>) +
        dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Import>([](const ParseState& st, const char* pos, std::string path) {
            if (Removed)
                st.error(pos, "'import' is removed; write 'using'");
            if (path.size() > 2 && path.front() == '<' && path.back() != '>')
                st.error(pos, "using <...> needs a closing '>'");
            else if (path == "<>" || path.empty())
                st.error(pos, "using needs a header name");
            ast::Import im;
            im.path = std::move(path);
            im.pos = st.offset(pos);
            im.line = st.map->line(im.pos);
            return im;
        }),
        lexy::parse_state, lexy::values);
};
using import_p = import_t<false>;
using old_import_p = import_t<true>;

// ---------------------------------------------------------------- members
// A bad argument of an attribute. It reports one error and skips to the next "," or ")".
// The value is an empty string. The argument count stays right, so no second error follows.
struct bad_attr_arg {
    static constexpr auto rule =
        dsl::position + dsl::loop(dsl::peek(dsl::lit_c<','> / dsl::lit_c<')'>) >> dsl::break_ |
                                  dsl::eof >> dsl::break_ | dsl::else_ >> any_unit);
    static constexpr auto value = lexy::bind(
        lexy::callback<std::string>([](const ParseState& st, const char* pos) {
            st.error(pos, e_alias_string::name);
            return std::string();
        }),
        lexy::parse_state, lexy::values);
};

// The strings of an attribute: "(" string { "," string } ")".
struct attr_args {
    static constexpr auto rule =
        dsl::lit_c<'('> +
        dsl::list(dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_value> |
                      dsl::else_ >> dsl::p<bad_attr_arg>,
                  dsl::sep(dsl::lit_c<','>)) +
        dsl::lit_c<')'>;
    static constexpr auto value = lexy::as_list<std::vector<std::string>>;
};

// One attribute in the list: a name and optional strings in parentheses.
struct attr_p {
    static constexpr auto rule =
        dsl::position + dsl::p<name_p<e_attr_name>> +
        dsl::opt(dsl::peek(dsl::lit_c<'('>) >> dsl::p<attr_args>);
    static constexpr auto value = lexy::callback<AttrItem>(
        [](const char* pos, std::string name, lexy::nullopt) {
            return AttrItem{pos, std::move(name), {}};
        },
        [](const char* pos, std::string name, std::vector<std::string>&& args) {
            return AttrItem{pos, std::move(name), std::move(args)};
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
                else if (it.args.size() != 1)
                    st.error(it.pos, "attribute 'alias' takes 1 arguments");
                else if (a.has_alias)
                    st.error(it.pos, "duplicate attribute 'alias'");
                else {
                    a.has_alias = true;
                    a.alias = std::move(it.args[0]);
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

// "Type name". The old form "name: type" is an error.
struct param_p {
    static constexpr auto rule =
        dsl::peek(old_param_ahead) >> dsl::error<e_old_param> |
        dsl::else_ >> (dsl::p<pass_mode_p> + dsl::position + dsl::p<scan_param_type> +
                       dsl::p<name_p<e_param_name>> + dsl::if_(dsl::lit_c<','>));
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Param>(
            [](const ParseState& st, ast::PassMode mode, const char* pos, Span type,
               std::string name) {
                check_name(st, pos, name);
                if (only_blank(span_text(st, type))) {
                    st.error(pos, "a parameter needs a type; write 'Type name'");
                    return ast::Param{std::move(name), "", mode};
                }
                return ast::Param{std::move(name), type_of(st, type), mode};
            }),
        lexy::parse_state, lexy::values);
};

struct params_p {
    static constexpr auto rule =
        dsl::lit_c<'('> + dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::p<param_p>);
    static constexpr auto value = lexy::as_list<std::vector<ast::Param>>;
};

// The return type after "->". The form is removed. The parse reads it to go on.
struct ArrowRet {
    const char* pos = nullptr;  // the "->"
    Span type;
};

struct arrow_p : lexy::token_production {
    static constexpr auto rule =
        dsl::position + LEXY_LIT("->") + dsl::p<scan_type<ret_term>>;
    static constexpr auto value = lexy::callback<ArrowRet>(
        [](const char* pos, Span type) { return ArrowRet{pos, type}; });
};

// The part of a method after the name: params, an old "->" return type and body.
constexpr auto kw_super = LEXY_KEYWORD("super", id);

// ": super(args)". The args stay a raw span. The call is valid in a constructor only.
struct super_call_p {
    static constexpr auto rule =
        dsl::lit_c<':'> + dsl::position + (kw_super | dsl::else_ >> dsl::error<e_super>) +
        dsl::p<scan_until<paren_cfg>> + dsl::lit_c<')'>;
    static constexpr auto value = lexy::callback<SuperCall>(
        [](const char* pos, Span args) { return SuperCall{pos, args}; });
};

struct method_tail {
    static constexpr auto rule = dsl::p<params_p> +
                                 dsl::opt(dsl::peek(LEXY_LIT("->")) >> dsl::p<arrow_p>) +
                                 dsl::opt(dsl::peek(dsl::lit_c<':'>) >> dsl::p<super_call_p>) +
                                 dsl::p<scan_balanced>;
    static constexpr auto value = lexy::bind(
        lexy::callback<MethodRest>([](const ParseState& st, auto&& params, auto&& arrow, auto&& sup,
                                      Span body) {
            MethodRest r;
            if constexpr (!std::is_same_v<std::decay_t<decltype(params)>, lexy::nullopt>)
                r.params = std::move(params);
            if constexpr (!std::is_same_v<std::decay_t<decltype(arrow)>, lexy::nullopt>) {
                r.arrow = arrow.pos;
                r.ret = type_of(st, arrow.type);
            }
            if constexpr (!std::is_same_v<std::decay_t<decltype(sup)>, lexy::nullopt>) {
                r.super_pos = sup.pos;
                r.super_args = sup.args;
            }
            r.body = body;
            return r;
        }),
        lexy::parse_state, lexy::values);
};

// The removed form "function name(...) -> Ret { ... }". The parse reads it to go on.
struct method_rest {
    static constexpr auto rule = dsl::p<name_p<e_method_name>> + dsl::p<method_tail>;
    static constexpr auto value =
        lexy::callback<MethodRest>([](std::string name, MethodRest&& r) {
            r.name = std::move(name);
            return r;
        });
};

// The part of a field after the name: an optional initializer and the ";".
struct field_tail {
    static constexpr auto rule =
        dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<init_cfg>>) + dsl::lit_c<';'>;
    static constexpr auto value = lexy::callback<FieldRest>([](auto init) {
        FieldRest f;
        if constexpr (!std::is_same_v<decltype(init), lexy::nullopt>) {
            f.has_init = true;
            f.init = init;
        }
        return f;
    });
};

// One accessor of a property: [access] ("get" | "set") (";" | "{ body }").
constexpr auto access_set = dsl::literal_set(kw_public, kw_private, kw_protected);

struct mod_item_access : lexy::token_production {
    static constexpr auto rule = dsl::capture(access_set);
    static constexpr auto value = lexy::callback<ModTok>(
        [](auto lex) { return ModTok{lex.begin(), std::string(lex.begin(), lex.end())}; });
};

struct accessor_p {
    static constexpr auto rule =
        dsl::opt(dsl::peek(access_set) >> dsl::p<mod_item_access>) + dsl::position +
        dsl::must(dsl::literal_set(kw_get, kw_set)).template error<e_accessor> +
        (dsl::peek(dsl::lit_c<'{'>) >> dsl::p<scan_balanced> | dsl::else_ >> dsl::lit_c<';'>);
    static constexpr auto value = lexy::callback<AccessorItem>(
        [](auto&& acc, const char* pos, auto&&... body) {
            AccessorItem a;
            a.pos = pos;
            a.is_get = *pos == 'g';
            if constexpr (!std::is_same_v<std::decay_t<decltype(acc)>, lexy::nullopt>) {
                a.has_access = true;
                a.access = acc.word == "public"    ? ast::Access::Public
                           : acc.word == "private" ? ast::Access::Private
                                                   : ast::Access::Protected;
            }
            if constexpr (sizeof...(body) == 1) {
                a.has_body = true;
                ((a.body = body), ...);
            }
            return a;
        });
};

struct accessors_p {
    static constexpr auto rule =
        dsl::lit_c<'{'> + dsl::terminator(dsl::lit_c<'}'>).opt_list(dsl::p<accessor_p>);
    static constexpr auto value = lexy::as_list<std::vector<AccessorItem>>;
};

// The part of a property after the name: the accessors and an optional "= init;".
struct prop_tail {
    static constexpr auto rule =
        dsl::position + dsl::p<accessors_p> +
        dsl::opt(dsl::peek(dsl::lit_c<'='>) >> (dsl::p<scan_until<init_cfg>> + dsl::lit_c<';'>));
    static constexpr auto value =
        lexy::callback<PropRest>([](const char* open, auto&& items, auto&& init) {
            PropRest r;
            r.open = open;
            if constexpr (!std::is_same_v<std::decay_t<decltype(items)>, lexy::nullopt>)
                r.accessors = std::move(items);
            if constexpr (!std::is_same_v<std::decay_t<decltype(init)>, lexy::nullopt>) {
                r.has_init = true;
                r.init = init;
            }
            return r;
        });
};

// The name of a member: an identifier before "(", "=", ";" or "{" (a property).
constexpr auto member_name_ahead =
    id + dsl::whitespace(ws_rule) +
    (dsl::lit_c<'('> / dsl::lit_c<'='> / dsl::lit_c<';'> / dsl::lit_c<'{'>);

// Matches the type of a member. The scan stops before the name.
// The name is the last identifier before "(", "=", ";" or "{" at depth zero.
// An identifier after a "." is part of the type, so "std.string" is a type and not a name.
// The span is empty when the member has no type.
struct scan_member_type : lexy::token_production {
    static constexpr auto rule =
        dsl::position +
        dsl::loop(dsl::peek(member_name_ahead) >> dsl::break_ |
                  dsl::peek(dsl::literal_set(dsl::lit_c<'('>, dsl::lit_c<'='>, dsl::lit_c<';'>,
                                             dsl::lit_c<'{'>, dsl::lit_c<'}'>)) >>
                      dsl::break_ |
                  type_ops | dsl::lit_c<'.'> >> dsl::if_(dsl::token(id)) | dsl::token(id) |
                  dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> dsl::p<type_item>) +
        dsl::position;
    static constexpr auto value = span_value;
};

// "Type name(params) { body }" or "Type name = init;" or "Type name;".
struct typed_member {
    static constexpr auto rule =
        dsl::position + dsl::p<scan_member_type> + dsl::position + dsl::p<name_p<e_member_name>> +
        (dsl::peek(dsl::lit_c<'('>) >> dsl::p<method_tail> |
         dsl::peek(dsl::lit_c<'{'>) >> dsl::p<prop_tail> | dsl::else_ >> dsl::p<field_tail>);
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>(
            [](const ParseState& st, const char* start, Span type, const char* name_pos,
               std::string name, PropRest&& rest) {
                Member m;
                m.kind = Member::IsProp;
                ast::Property& p = m.prop;
                check_name(st, name_pos, name);
                if (only_blank(span_text(st, type)))
                    st.error(start, "a property needs a type; write 'Type Name { get; set; }'");
                p.name = std::move(name);
                p.type = type_of(st, type);
                for (const AccessorItem& a : rest.accessors) {
                    const char* word = a.is_get ? "get" : "set";
                    if (a.is_get ? p.has_get : p.has_set) {
                        st.error(a.pos, std::string("duplicate '") + word + "' accessor");
                        continue;
                    }
                    (a.is_get ? p.has_get : p.has_set) = true;
                    (a.is_get ? p.get_pos : p.set_pos) = st.offset(a.pos);
                    (a.is_get ? m.get_has_access : m.set_has_access) = a.has_access;
                    (a.is_get ? p.get_access : p.set_access) = a.access;
                    if (a.has_body) {
                        (a.is_get ? p.get_body : p.set_body).emplace();
                        (a.is_get ? p.get_span : p.set_span) = a.body;
                    }
                }
                if (!p.has_get && !p.has_set)
                    st.error(rest.open, "a property needs a 'get' or a 'set' accessor");
                const bool custom_get = p.get_body.has_value(), custom_set = p.set_body.has_value();
                if (p.has_get && p.has_set && custom_get != custom_set)
                    st.error(rest.open, "a property cannot mix auto and custom accessors");
                if (rest.has_init) {
                    p.has_init = true;
                    p.init_span = rest.init;
                    if (!p.is_auto())
                        st.error(st.base + rest.init.begin - 1,
                                 "only an auto property can have an initializer");
                    else if (only_blank(span_text(st, rest.init)))
                        st.error(st.base + rest.init.begin, "expected an initializer");
                }
                p.pos = st.offset(name_pos);
                p.line = st.map->line(p.pos);
                return m;
            },
            [](const ParseState& st, const char* start, Span type, const char* name_pos,
               std::string name, MethodRest&& rest) {
                Member m;
                check_name(st, name_pos, name);
                if (only_blank(span_text(st, type))) {
                    // No type: a constructor, when the name is the name of the class. The step
                    // that adds the members to the class checks the name.
                    m.kind = Member::IsCtor;
                    ast::Ctor& c = m.ctor;
                    m.ctor_name = std::move(name);
                    c.params = std::move(rest.params);
                    c.body_span = rest.body;
                    c.pos = st.offset(name_pos);
                    c.line = st.map->line(c.pos);
                    c.end_pos = rest.body.end;
                    c.end_line = st.map->line(c.end_pos);
                    if (rest.arrow)
                        st.error(rest.arrow, "'->' return type is removed; write the type before the name");
                    if (rest.super_pos) {
                        c.has_super = true;
                        c.super_span = rest.super_args;
                        c.super_pos = st.offset(rest.super_pos);
                    }
                    return m;
                }
                m.kind = Member::IsMethod;
                ast::Method& me = m.method;
                std::string ret = type_of(st, type);
                if (rest.super_pos)
                    st.error(rest.super_pos, "only a constructor can call 'super(...)' after the parameters");
                if (rest.arrow)
                    st.error(rest.arrow,
                             "'->' return type is removed; write the type before the name");
                me.name = std::move(name);
                me.params = std::move(rest.params);
                me.ret = ret == "void" ? std::string{} : std::move(ret);
                me.body_span = rest.body;
                me.pos = st.offset(start);
                me.line = st.map->line(me.pos);
                me.end_pos = rest.body.end;
                me.end_line = st.map->line(me.end_pos);
                return m;
            },
            [](const ParseState& st, const char* start, Span type, const char* name_pos,
               std::string name, FieldRest&& rest) {
                Member m;
                m.kind = Member::IsField;
                ast::Field& f = m.field;
                check_name(st, name_pos, name);
                if (only_blank(span_text(st, type)))
                    st.error(start, "a field needs a type; write 'Type name;'");
                f.name = std::move(name);
                f.type = type_of(st, type);
                if (rest.has_init) {
                    f.has_init = true;
                    f.init_span = rest.init;
                    if (only_blank(span_text(st, rest.init)))
                        st.error(st.base + rest.init.begin, "expected an initializer");
                }
                f.pos = st.offset(name_pos);
                f.line = st.map->line(f.pos);
                return m;
            }),
        lexy::parse_state, lexy::values);
};

// The junk token for the error text: a word or one code point.
constexpr auto junk = dsl::while_one(dsl::ascii::word) | dsl::else_ >> any_unit;

// The removed form "name: Type;". The parse reads the name. The sync skips the rest.
constexpr auto old_field_ahead =
    id + dsl::whitespace(ws_rule) + dsl::lit_c<':'> + dsl::peek_not(dsl::lit_c<':'>);

constexpr auto modifier_set =
    dsl::literal_set(kw_override, kw_virtual, kw_new, kw_public, kw_private, kw_protected);

struct mod_item : lexy::token_production {
    static constexpr auto rule = dsl::position + dsl::capture(modifier_set);
    static constexpr auto value = lexy::callback<ModTok>(
        [](const char* pos, auto lex) { return ModTok{pos, std::string(lex.begin(), lex.end())}; });
};

// One or more modifiers, in any order.
struct mods_p {
    static constexpr auto rule = dsl::list(dsl::peek(modifier_set) >> dsl::p<mod_item>);
    static constexpr auto value = lexy::as_list<std::vector<ModTok>>;
};

// "~Name() { body }". The add step checks the name.
struct dtor_p {
    static constexpr auto rule = dsl::position + dsl::lit_c<'~'> + dsl::position +
                                 dsl::p<name_p<e_member_name>> + dsl::p<method_tail>;
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>([](const ParseState& st, const char* start, const char* name_pos,
                                  std::string name, MethodRest&& rest) {
            Member m;
            m.kind = Member::IsDtor;
            ast::Method& me = m.method;
            if (!rest.params.empty())
                st.error(name_pos, "a destructor has no parameters");
            if (rest.super_pos)
                st.error(rest.super_pos, "only a constructor can call 'super(...)'");
            m.ctor_name = name;
            me.name = "~" + name;
            me.body_span = rest.body;
            me.pos = st.offset(start);
            me.line = st.map->line(me.pos);
            me.end_pos = rest.body.end;
            me.end_line = st.map->line(me.end_pos);
            return m;
        }),
        lexy::parse_state, lexy::values);
};

struct member_p {
    static constexpr auto rule =
        dsl::position +
        dsl::opt(dsl::peek(dsl::lit_c<'@'> / dsl::lit_c<'['>) >> dsl::p<annot_p>) +
        dsl::opt(dsl::peek(modifier_set) >> dsl::p<mods_p>) +
        (dsl::eof >> dsl::error<e_unexpected_eof> |
         dsl::capture(kw_function) >> dsl::p<method_rest> |
         dsl::peek(dsl::lit_c<'~'>) >> dsl::p<dtor_p> |
         dsl::peek(old_field_ahead) >> dsl::error<e_old_field>(id) |
         dsl::peek(id) >> dsl::p<typed_member> | dsl::else_ >> dsl::error<e_member>(junk));
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>(
            [](const ParseState& st, const char*, const auto& ann, const auto& mods,
               const auto& kw, MethodRest&& rest) {
                Member m;
                m.kind = Member::IsMethod;
                ast::Method& me = m.method;
                st.error(kw.begin(), "'function' is removed; write 'Ret name(...)'");
                me.name = std::move(rest.name);
                me.params = std::move(rest.params);
                me.ret = std::move(rest.ret);
                me.body_span = rest.body;
                me.pos = st.offset(kw.begin());
                me.line = st.map->line(me.pos);
                me.end_pos = rest.body.end;
                me.end_line = st.map->line(me.end_pos);
                return finish_member(st, ann, mods, std::move(m));
            },
            [](const ParseState& st, const char*, const auto& ann, const auto& mods, Member&& m) {
                return finish_member(st, ann, mods, std::move(m));
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

// The bases of a class: "Base" or "Base, IFace, ...". The first name is the primary base. The
// other names are native interface bases (design 4.4).
struct bases_p {
    static constexpr auto rule = dsl::list(dsl::p<base_p>, dsl::sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<std::string>>;
};

// The attributes before a class or a stub. The parser checks the names and the argument counts.
struct ClassAttrs {
    const char* pos = nullptr;  // the "["
    bool is_native = false, is_value = false;
    std::string handle, default_handle, intrusive_add, intrusive_release;
    std::string alias;  // [alias("old")]: valid on a module variable only
    bool has_alias = false;
    const char* handle_pos = nullptr;  // the "handle" name, or null
    std::vector<std::pair<const char*, std::string>> used;  // the valid attributes
};

// The expected count of arguments of a class attribute, or -1 for an unknown name.
int class_attr_arity(const std::string& name)
{
    if (name == "native" || name == "value") return 0;
    if (name == "handle" || name == "default_handle" || name == "alias") return 1;
    if (name == "intrusive") return 2;
    return -1;
}

// "[" attribute { "," attribute } "]" before "class".
struct class_attrs_p {
    static constexpr auto rule = dsl::position + dsl::lit_c<'['> + dsl::p<attr_list>;
    static constexpr auto value = lexy::bind(
        lexy::callback<ClassAttrs>([](const ParseState& st, const char* pos,
                                      std::vector<AttrItem>&& items) {
            ClassAttrs a;
            a.pos = pos;
            for (AttrItem& it : items) {
                const int arity = class_attr_arity(it.name);
                bool dup = false;
                for (const auto& u : a.used) dup = dup || u.second == it.name;
                if (arity < 0) {
                    st.error(it.pos, "unknown attribute '" + it.name + "'");
                    continue;
                }
                if (static_cast<size_t>(arity) != it.args.size()) {
                    st.error(it.pos, "attribute '" + it.name + "' takes " +
                                         std::to_string(arity) + " arguments");
                    continue;
                }
                if (dup) {
                    st.error(it.pos, "duplicate attribute '" + it.name + "'");
                    continue;
                }
                a.used.emplace_back(it.pos, it.name);
                if (it.name == "native") a.is_native = true;
                else if (it.name == "value") a.is_value = true;
                else if (it.name == "alias") {
                    a.alias = std::move(it.args[0]);
                    a.has_alias = true;
                }
                else if (it.name == "handle") {
                    a.handle = std::move(it.args[0]);
                    a.handle_pos = it.pos;
                } else if (it.name == "default_handle")
                    a.default_handle = std::move(it.args[0]);
                else {
                    a.intrusive_add = std::move(it.args[0]);
                    a.intrusive_release = std::move(it.args[1]);
                }
            }
            return a;
        }),
        lexy::parse_state, lexy::values);
};

// The "*" in a stub name.
struct star_tok : lexy::token_production {
    static constexpr auto rule = dsl::capture(dsl::lit_c<'*'>);
    static constexpr auto value = lexy::as_string<std::string>;
};

// The suffix "<*>" of a stub name.
struct star_suffix_tok : lexy::token_production {
    static constexpr auto rule = dsl::capture(LEXY_LIT("<*>"));
    static constexpr auto value = lexy::as_string<std::string>;
};

// One part of a stub name: a name or "*".
struct stub_part {
    static constexpr auto rule =
        dsl::peek(dsl::lit_c<'*'>) >> dsl::p<star_tok> | dsl::else_ >> dsl::p<name_p<e_class_name>>;
    static constexpr auto value = lexy::forward<std::string>;
};

// The parts of a stub name. The text joins the parts with "::".
struct stub_dotted {
    static constexpr auto rule = dsl::list(dsl::p<stub_part>, dsl::sep(dsl::lit_c<'.'>));
    static constexpr auto value = lexy::fold_inplace<std::string>(
        std::string{}, [](std::string& acc, std::string&& part) {
            if (!acc.empty()) acc += "::";
            acc += part;
        });
};

// "*", or a dotted name with an optional suffix "<*>". A ".*" at the end becomes "::*".
struct stub_name_p {
    static constexpr auto rule =
        dsl::p<stub_dotted> + dsl::opt(dsl::peek(LEXY_LIT("<*>")) >> dsl::p<star_suffix_tok>);
    static constexpr auto value = lexy::callback<std::string>(
        [](std::string&& name, lexy::nullopt) { return std::move(name); },
        [](std::string&& name, std::string&& suffix) { return std::move(name) + suffix; });
};

void add_members(const ParseState&, ast::ClassDef&, lexy::nullopt) {}
void add_members(const ParseState& st, ast::ClassDef& c, std::vector<Member>&& members)
{
    for (Member& m : members) {
        if (m.kind == Member::IsDtor) {
            if (m.ctor_name == c.name)
                c.dtors.push_back(std::move(m.method));
            else
                st.error(st.base + m.method.pos, "the name of a destructor is '~" + c.name + "'");
        } else if (m.kind == Member::IsCtor) {
            if (m.ctor_name == c.name)
                c.ctors.push_back(std::move(m.ctor));
            else
                st.error(st.base + m.ctor.pos, "a method needs a return type; write 'Ret name(...)'");
        } else if (m.kind == Member::IsMethod)
            c.methods.push_back(std::move(m.method));
        else if (m.kind == Member::IsField)
            c.fields.push_back(std::move(m.field));
        else if (m.kind == Member::IsProp)
            c.props.push_back(std::move(m.prop));
    }
}

const ClassAttrs* class_attrs_of(lexy::nullopt) { return nullptr; }
const ClassAttrs* class_attrs_of(const ClassAttrs& a) { return &a; }

// The part of a class, a struct or a stub after the keyword: the name, then ";" (a stub) or
// the base and the members. The value is the position and the name first.
constexpr auto class_rest =
    dsl::position + dsl::p<stub_name_p> +
    (dsl::lit_c<';'> >> dsl::nullopt |
     dsl::else_ >> (dsl::opt(dsl::lit_c<':'> >> dsl::p<bases_p>) + dsl::lit_c<'{'> + dsl::p<members_p>));

// [attributes] class name ";" is a stub. [attributes] class name ":" Base "{" members "}" is a class.
// "struct" is the same as "[value] class". Else the declaration is a module variable:
// [alias("old")] Type name [= init];
struct class_p {
    static constexpr auto rule =
        dsl::position + dsl::opt(dsl::peek(dsl::lit_c<'['>) >> dsl::p<class_attrs_p>) +
        (dsl::peek(kw_class) >> (dsl::capture(kw_class) + class_rest) |
         dsl::peek(kw_struct) >> (dsl::capture(kw_struct) + class_rest) |
         dsl::else_ >> dsl::p<typed_member>);
    static constexpr auto value = lexy::bind(
        lexy::callback<TopItem>(
            [](const ParseState& st, const char* pos, const auto& attrs, const auto& kw,
               const char* name_pos, std::string&& name, lexy::nullopt) {
                const ClassAttrs* a = class_attrs_of(attrs);
                const bool is_struct = std::distance(kw.begin(), kw.end()) == 6;
                TopItem t;
                t.kind = TopItem::IsStub;
                ast::Stub& s = t.stub;
                s.name = std::move(name);
                s.pos = st.offset(pos);
                s.line = st.map->line(s.pos);
                if (a) {
                    s.is_native = a->is_native;
                    s.is_value = a->is_value;
                    s.handle = a->handle;
                    s.default_handle = a->default_handle;
                    s.intrusive_add = a->intrusive_add;
                    s.intrusive_release = a->intrusive_release;
                    if (a->is_value && a->handle_pos)
                        st.error(a->handle_pos, "'value' and 'handle' are exclusive");
                    if (a->has_alias)
                        st.error(a->pos, "attribute 'alias' is not allowed on a stub");
                }
                if (is_struct) st.error(pos, "a native stub is declared with 'class', not 'struct'");
                else if (!s.is_native) st.error(pos, "a stub needs the attribute 'native'");
                (void)name_pos;
                return t;
            },
            [](const ParseState& st, const char* pos, const auto& attrs, const auto& kw,
               const char* name_pos, std::string&& name, auto&& base, auto&& members) {
                const ClassAttrs* a = class_attrs_of(attrs);
                const bool is_struct = std::distance(kw.begin(), kw.end()) == 6;
                TopItem t;
                t.kind = TopItem::IsClass;
                ast::ClassDef& c = t.cls;
                if (name.find_first_of("*<:") != std::string::npos)
                    st.error(name_pos, "a script class needs a plain name");
                c.name = std::move(name);
                if constexpr (!std::is_same_v<std::decay_t<decltype(base)>, lexy::nullopt>) {
                    if (!base.empty()) c.base = std::move(base.front());
                    for (size_t i = 1; i < base.size(); ++i) c.interfaces.push_back(std::move(base[i]));
                }
                c.pos = st.offset(pos);
                c.line = st.map->line(c.pos);
                // A struct is a [value] class (design 5.3, 7.10.1).
                c.is_value = is_struct;
                if (a) {
                    c.is_value = c.is_value || a->is_value;
                    for (const auto& u : a->used)
                        if (u.second != "value")
                            st.error(u.first,
                                     "attribute '" + u.second + "' is not allowed on a script class");
                }
                add_members(st, c, std::move(members));
                return t;
            },
            [](const ParseState& st, const char* pos, const auto& attrs, Member&& m) {
                const ClassAttrs* a = class_attrs_of(attrs);
                TopItem t;
                if (m.kind != Member::IsField) {
                    st.error(pos, m.kind == Member::IsMethod
                                      ? "a function must be a member of a class"
                                      : "expected 'class', 'struct', 'enum' or a variable 'Type name = init;'");
                    return t;
                }
                t.kind = TopItem::IsVar;
                t.var = std::move(m.field);
                if (a) {
                    t.var.alias = a->alias;
                    for (const auto& u : a->used)
                        if (u.second != "alias")
                            st.error(u.first, "attribute '" + u.second +
                                                  "' is not allowed on a module variable");
                }
                return t;
            }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- enums
// The end of an enumerator value: a "," or the closing brace at depth zero.
struct enum_value_cfg {
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = dsl::lit_c<','> / dsl::lit_c<'}'>;
};

struct enumerator_p {
    static constexpr auto rule =
        dsl::position + dsl::p<name_p<e_enumerator>> +
        dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<enum_value_cfg>>);
    static constexpr auto value = lexy::bind(
        lexy::callback<ast::Enumerator>(
            [](const ParseState& st, const char* pos, std::string&& name, lexy::nullopt) {
                ast::Enumerator e;
                e.name = std::move(name);
                e.pos = st.offset(pos);
                e.line = st.map->line(e.pos);
                return e;
            },
            [](const ParseState& st, const char* pos, std::string&& name, Span value) {
                ast::Enumerator e;
                e.name = std::move(name);
                e.pos = st.offset(pos);
                e.line = st.map->line(e.pos);
                e.has_value = true;
                e.value_span = value;
                if (only_blank(span_text(st, value)))
                    st.error(st.base + value.begin, "expected an enumerator value");
                return e;
            }),
        lexy::parse_state, lexy::values);
};

struct enumerators_p {
    static constexpr auto rule = dsl::lit_c<'{'> +
                                 dsl::terminator(dsl::lit_c<'}'>)
                                     .opt_list(dsl::p<enumerator_p>, dsl::trailing_sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<ast::Enumerator>>;
};

// "enum Name { A, B = 5 }" or "enum Name : type { ... }".
struct enum_p {
    static constexpr auto rule =
        dsl::position + kw_enum + dsl::position + dsl::p<name_p<e_enum_name>> +
        dsl::opt(dsl::lit_c<':'> >> (dsl::position + dsl::p<scan_type<ret_term>>)) +
        dsl::p<enumerators_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<TopItem>(
            [](const ParseState& st, const char* pos, const char*, std::string&& name,
               lexy::nullopt, std::vector<ast::Enumerator>&& items) {
                TopItem t;
                t.kind = TopItem::IsEnum;
                t.en.name = std::move(name);
                t.en.pos = st.offset(pos);
                t.en.line = st.map->line(t.en.pos);
                t.en.items = std::move(items);
                return t;
            },
            [](const ParseState& st, const char* pos, const char*, std::string&& name,
               const char* type_pos, Span type, std::vector<ast::Enumerator>&& items) {
                TopItem t;
                t.kind = TopItem::IsEnum;
                t.en.name = std::move(name);
                t.en.pos = st.offset(pos);
                t.en.line = st.map->line(t.en.pos);
                t.en.items = std::move(items);
                t.en.underlying = type_of(st, type);
                t.en.underlying_pos = st.offset(type_pos);
                return t;
            }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- file
struct decl {
    static constexpr auto rule =
        dsl::peek(kw_using) >> dsl::p<import_p> |
        dsl::peek(kw_import) >> dsl::p<old_import_p> |
        dsl::peek(kw_enum) >> dsl::p<enum_p> |
        dsl::peek(dsl::literal_set(dsl::lit_c<'['>, kw_class, kw_struct)) >> dsl::p<class_p> |
        dsl::peek(id) >> dsl::p<class_p> |
        dsl::else_ >> dsl::error<e_top>(junk);
    static constexpr auto value = lexy::callback<TopItem>(
        [](ast::Import&& im) {
            TopItem t;
            t.kind = TopItem::IsImport;
            t.import = std::move(im);
            return t;
        },
        [](TopItem&& t) { return std::move(t); });
};

// Skips a broken declaration. It stops before the next "using", "import" or "class".
struct sync_decl : lexy::token_production {
    static constexpr auto rule = dsl::loop(
        dsl::eof >> dsl::break_ |  // the end of the file is not a new error after a broken decl
        dsl::peek(dsl::literal_set(kw_using, kw_import, kw_class, kw_struct, kw_enum)) >> dsl::break_ |
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
            else if (t.kind == TopItem::IsStub)
                script.stubs.push_back(std::move(t.stub));
            else if (t.kind == TopItem::IsEnum)
                script.enums.push_back(std::move(t.en));
            else if (t.kind == TopItem::IsVar)
                script.vars.push_back(std::move(t.var));
        }
    }
    // The span-then-call step: parse each body and each initializer.
    const ParseInput in{text, &map};
    for (ast::EnumDef& e : script.enums)
        for (ast::Enumerator& m : e.items)
            if (m.has_value && !only_blank(span_text(st, m.value_span)))
                m.value = parse_expression(in, m.value_span, diag);
    for (ast::Field& f : script.vars)
        if (f.has_init && !only_blank(span_text(st, f.init_span)))
            f.init = parse_expression(in, f.init_span, diag);
    for (ast::ClassDef& c : script.classes) {
        for (ast::Method& m : c.methods) m.body = parse_block(in, m.body_span, diag);
        for (ast::Method& d : c.dtors) d.body = parse_block(in, d.body_span, diag);
        for (ast::Ctor& k : c.ctors) {
            k.body = parse_block(in, k.body_span, diag);
            if (k.has_super) k.super_args = parse_expression_list(in, k.super_span, diag);
        }
        for (ast::Field& f : c.fields)
            if (f.has_init && !only_blank(span_text(st, f.init_span)))
                f.init = parse_expression(in, f.init_span, diag);
        for (ast::Property& p : c.props) {
            if (p.get_body) p.get_body = parse_block(in, p.get_span, diag);
            if (p.set_body) p.set_body = parse_block(in, p.set_span, diag);
            if (p.has_init && !only_blank(span_text(st, p.init_span)))
                p.init = parse_expression(in, p.init_span, diag);
        }
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

}  // namespace sppc
