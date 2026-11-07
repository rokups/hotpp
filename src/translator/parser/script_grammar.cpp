// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The grammar of the script structure: imports, classes, fields and methods.
//
// The grammar finds the end of each method body and of each field initializer and stores the
// span. After the parse, parse_script calls parse_block and parse_expression on each span.
// So one call gives the errors of the whole file. The function sorts them by position.
// Error recovery works at two levels. A broken member skips to the next ";" or "}" or to the next member. A broken
// using declaration or class skips to the next "using" or "class".
#include "expression.h"
#include "lexy_common.hpp"
#include "parser.h"
#include "statement.h"
#include "../operator_names.h"
#include "../std_ns.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hotc
{
namespace
{

using namespace parser; // NOLINT: the grammar uses the names of lexy_common.hpp

// ---------------------------------------------------------------- error texts
HOT_ERROR_TAG(e_top, "expected 'using', 'class', 'struct', 'interface', 'enum' or a variable");
HOT_ERROR_TAG(e_enum_name, "expected an enum name");
HOT_ERROR_TAG(e_enumerator, "expected an enumerator name");
HOT_ERROR_TAG(e_member, "expected a member: 'Type name;' or 'Ret name(...)'");
HOT_ERROR_TAG(e_member_name, "expected a member name");
HOT_ERROR_TAG(e_op_symbol, "expected an operator symbol after 'operator'");
HOT_ERROR_TAG(e_op_params, "an operator needs a parameter list");
HOT_ERROR_TAG(e_class_name, "expected a class name");
HOT_ERROR_TAG(e_abstract_target, "expected 'class' after 'abstract'");
HOT_ERROR_TAG(e_iface_name, "expected an interface name");
HOT_ERROR_TAG(e_iface_kw, "expected 'interface'");
HOT_ERROR_TAG(e_base_name, "expected a base class name");
HOT_ERROR_TAG(e_field_name, "expected a field name");
HOT_ERROR_TAG(e_param_name, "expected a parameter name");
HOT_ERROR_TAG(e_attr_name, "expected an attribute name");
HOT_ERROR_TAG(e_alias_string, "expected a string in [alias(...)]");
HOT_ERROR_TAG(e_import_name, "using needs a header name");
HOT_ERROR_TAG(e_import_arg, "using needs a string or <header>");
HOT_ERROR_TAG(e_import_close, "using <...> needs a closing '>'");
HOT_ERROR_TAG(e_accessor, "expected 'get' or 'set'");
HOT_ERROR_TAG(e_base, "expected 'base(...)' after ':' of a constructor");

// The text of a span.
std::string_view span_text(const ParseState& st, Span sp)
{
    return std::string_view(st.base + sp.begin, sp.end - sp.begin);
}

// The type text of a span. A "." is a scope and becomes "::". A "::" is an error.
// A "T[]" is an error when "view_ok" is false (a field or a property).
std::string type_of(const ParseState& st, Span sp, bool view_ok = true)
{
    return type_of_span(st, sp, view_ok);
}

// ---------------------------------------------------------------- values
// The attributes before a field. The value is the alias, or empty.
struct Annot
{
    const char* pos = nullptr; // the "[" or the "@"
    std::string alias;
    bool has_alias = false;
    const char* weak_pos = nullptr; // the name of "[weak]", or null
    const char* byval_pos = nullptr; // the name of "[byval]", or null
    // The other names: script attribute tags with optional strings. Only a field accepts them.
    struct Tag
    {
        const char* pos;
        std::string name;
        std::vector<std::string> args;
    };
    std::vector<Tag> tags;
};

// A tag is valid only on a field. Other members and parameters report it as an unknown attribute.
void reject_member_tags(const ParseState& st, const Annot& a)
{
    for (const Annot::Tag& t : a.tags)
        st.error(t.pos, "unknown attribute '" + t.name + "'");
}

// One attribute: a name and a list of strings. The list can be empty.
struct AttrItem
{
    const char* pos = nullptr; // the name
    std::string name;
    std::vector<std::string> args;
};

// The base call of a constructor: ": base(args)".
struct BaseCall
{
    const char* pos = nullptr; // the "base" word
    Span args; // the text between the parentheses
};

struct MethodRest
{
    std::string name;
    std::vector<ast::Param> params;
    const char* base_pos = nullptr; // the "base" word of a base call, or null
    Span base_args;
    Span body;
    bool expr_body = false; // "=> e;": the body is the span of "e"
    bool is_const = false; // "F() const": the marker after the parameters. It is an error.
    const char* no_body_pos = nullptr; // "F();": the ";" that stands for the body
};

// The part of a field after the name.
struct FieldRest
{
    bool has_init = false;
    Span init;
};

// One accessor of a property: an optional access word, "get" or "set", and ";" or a body.
struct AccessorItem
{
    const char* pos = nullptr; // the "get" or "set" word
    bool is_get = false;
    bool has_body = false;
    Span body;
    bool has_access = false;
    ast::Access access = ast::Access::Public;
};

// The part of a property after the name.
struct PropRest
{
    const char* open = nullptr; // the "{"
    std::vector<AccessorItem> accessors;
    bool has_init = false;
    Span init;
    bool arrow = false; // "Type Name => e;"
    Span arrow_body;
};

struct Member
{
    enum Kind
    {
        None,
        IsMethod,
        IsField,
        IsProp,
        IsCtor,
        IsDtor,
        IsAlias,
        IsStaticAssert
    } kind = None;
    ast::StaticAssert sa;
    ast::TypeAlias alias;
    ast::Method method;
    ast::Ctor ctor; // a member with no type: the add step checks the name
    std::string ctor_name;
    ast::Field field;
    ast::Property prop;
    bool get_has_access = false, set_has_access = false; // an access word on the accessor
    // A method only: the words that a file-level function cannot have. The finish step sets them.
    const char* access_pos = nullptr; // the access word, or null
    const char* async_pos = nullptr; // the "async" word, or null
    const char* static_pos = nullptr; // the "static" word, or null
    const char* const_pos = nullptr; // a field only: the "const" word of a module constant, or null
    std::vector<std::pair<const char*, std::string>> kind_words; // "virtual", "override", "new", "abstract"
    const char* no_body_pos = nullptr; // the ";" of a method or a constructor with no body
};

// The words "get", "set", "async", "await", "interface", "extern", "ref", "out", "true" and "false" are reserved.
// They are not member or parameter names.
void check_name(const ParseState& st, const char* pos, const std::string& name)
{
    if (name == "get" || name == "set" || name == "async" || name == "await" || name == "interface"
        || name == "extern" || name == "ref" || name == "out" || name == "true" || name == "false")
        st.error(pos, "'" + name + "' is a reserved word");
}

// The value of the end ";" of a module attribute list: the position of the ";".
struct ModuleAttrsMark
{
    const char* pos = nullptr;
};

struct TopItem
{
    enum Kind
    {
        None,
        IsImport,
        IsClass,
        IsStub,
        IsEnum,
        IsVar,
        IsConst,
        IsHook,
        IsFn,
        IsModuleAttrs,
        IsUsingNs,
        IsAlias,
        IsInterface,
        IsStaticAssert
    } kind = None;
    ast::StaticAssert sa;
    bool is_private = false; // "private" before "class", "struct" or "enum" at file level
    ast::Import import;
    ast::UsingNs using_ns;
    ast::TypeAlias alias;
    ast::ClassDef cls;
    ast::InterfaceDef iface;
    ast::Stub stub;
    ast::EnumDef en;
    ast::Field var; // a module variable
    ast::Method hook; // a convert hook of a module variable
    ast::Method fn; // a free function
    ast::ModuleAttrs module_attrs; // "[std(\"x\")];"
};

const Annot* annot_of(lexy::nullopt)
{
    return nullptr;
}
const Annot* annot_of(const Annot& a)
{
    return &a;
}

// ---------------------------------------------------------------- modifiers
// One modifier word and its position.
struct ModTok
{
    const char* pos = nullptr;
    std::string word;
};

// The modifiers before a member. The parser checks the rules that do not depend on the kind.
struct Mods
{
    bool is_override = false, is_virtual = false, is_new = false, is_async = false, is_abstract = false;
    const char* abstract_pos = nullptr; // the "abstract" word, or null
    bool has_access = false;
    const char* access_pos = nullptr; // the access word, or null
    bool is_static = false;
    const char* static_pos = nullptr; // the "static" word, or null
    const char* const_pos = nullptr; // the "const" word, or null. Only a variable at file level can have it.
    ast::Access access = ast::Access::Public;
    std::vector<ModTok> kind_toks; // the "override", "virtual", "new" and "async" words
};

bool is_access_word(const std::string& w)
{
    return w == "public" || w == "private" || w == "protected";
}

// The error text for a C++ word that a script member cannot have, or null for any other word.
const char* unsupported_modifier(const std::string& w)
{
    if (w == "thread_local")
        return "thread_local members are not supported; use a module variable (a variable at file level)";
    if (w == "friend")
        return "'friend' is not supported in a script class";
    if (w == "mutable")
        return "'mutable' is not supported in a script class";
    if (w == "volatile")
        return "'volatile' is not supported in a script class";
    return nullptr;
}

// Builds the Mods from the word list. It reports duplicates and words that cannot combine.
Mods collect_mods(const ParseState& st, const std::vector<ModTok>& toks, bool is_op = false)
{
    Mods m;
    std::vector<const ModTok*> seen;
    const ModTok* access_tok = nullptr;
    const ModTok* kind_tok = nullptr;
    for (const ModTok& t : toks)
    {
        if (is_op && t.word == "static")
        {
            st.error(t.pos, "an operator cannot be 'static'");
            continue;
        }
        if (const char* why = unsupported_modifier(t.word))
        {
            st.error(t.pos, why);
            continue;
        }
        if (t.word == "const")
        {
            // The top-level item and the class body check the place of "const".
            if (m.const_pos)
            {
                st.error(t.pos, "duplicate modifier 'const'");
                continue;
            }
            m.const_pos = t.pos;
            continue;
        }
        if (t.word == "static")
        {
            // The kind of the member decides if "static" is valid: see finish_member.
            if (m.is_static)
            {
                st.error(t.pos, "duplicate modifier 'static'");
                continue;
            }
            m.is_static = true;
            m.static_pos = t.pos;
            continue;
        }
        bool dup = false;
        for (const ModTok* o : seen)
            dup = dup || o->word == t.word;
        if (dup)
        {
            st.error(t.pos, "duplicate modifier '" + t.word + "'");
            continue;
        }
        seen.push_back(&t);
        if (is_access_word(t.word))
        {
            if (access_tok)
            {
                st.error(t.pos, "'" + access_tok->word + "' and '" + t.word + "' cannot combine");
                continue;
            }
            access_tok = &t;
            m.has_access = true;
            m.access_pos = t.pos;
            m.access = t.word == "public" ? ast::Access::Public
                : t.word == "private"     ? ast::Access::Private
                                          : ast::Access::Protected;
        }
        else if (t.word == "abstract")
        {
            // "abstract" combines with "async" and with an access word only. The check of the
            // other words is in finish_member, where the kind of the member is known.
            m.abstract_pos = t.pos;
            m.is_abstract = true;
            m.kind_toks.push_back(t);
        }
        else if (t.word == "async")
        {
            // "async" combines with "virtual" and "override". A duplicate is an error above.
            m.kind_toks.push_back(t);
            m.is_async = true;
        }
        else
        {
            if (kind_tok)
            {
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

Mods mods_of(const ParseState&, lexy::nullopt, bool = false)
{
    return Mods{};
}
Mods mods_of(const ParseState& st, const std::vector<ModTok>& v, bool is_op = false)
{
    return collect_mods(st, v, is_op);
}

// Applies the attributes and modifiers before a member to the member.
template <typename Ann, typename ModsT>
Member finish_member(const ParseState& st, const Ann& ann, const ModsT& mods_in, Member&& m)
{
    if (m.kind == Member::IsStaticAssert)
        return std::move(m);
    if (const Annot* a = annot_of(ann))
    {
        if (a->byval_pos)
            st.error(a->byval_pos, "[byval] applies to parameters only");
        if (m.kind == Member::IsField)
        {
            m.field.alias = a->alias;
            m.field.weak = a->weak_pos != nullptr;
            for (const Annot::Tag& t : a->tags)
                m.field.attributes.push_back({t.name, t.args, st.offset(t.pos)});
        }
        else
        {
            reject_member_tags(st, *a);
            if (a->has_alias)
                st.error(a->pos, "[alias] applies to fields only");
            if (a->weak_pos)
                st.error(a->weak_pos, "[weak] applies to fields, parameters and local variables only");
        }
    }
    const bool is_op = m.kind == Member::IsMethod && !m.method.op.empty();
    const Mods mods = mods_of(st, mods_in, is_op);
    if (mods.const_pos)
    {
        if (m.kind == Member::IsField)
            m.const_pos = mods.const_pos;
        else
            st.error(mods.const_pos, "only a variable at file level can be 'const'");
    }
    if (mods.is_static)
    {
        // "static const T Name = value;" is a class constant. "static T Name;" is a static field
        // (gap G23). A static property has static accessors.
        const char* what = m.kind == Member::IsCtor ? "a constructor cannot be 'static'"
            : m.kind == Member::IsDtor ? "a destructor cannot be 'static'"
                                       : nullptr;
        if (what)
            st.error(mods.static_pos, what);
    }
    if (m.kind == Member::IsDtor)
    {
        m.method.access = mods.access;
        for (const ModTok& t : mods.kind_toks)
            st.error(t.pos, "'" + t.word + "' is not valid on a destructor");
        if (m.no_body_pos)
            st.error(m.no_body_pos, "a destructor needs a block body");
    }
    else if (m.kind == Member::IsCtor)
    {
        m.ctor.access = mods.access;
        for (const ModTok& t : mods.kind_toks)
            st.error(t.pos, t.word == "abstract" ? std::string("a constructor cannot be 'abstract'")
                                                 : "'" + t.word + "' is not valid on a constructor");
        if (m.no_body_pos)
            st.error(m.no_body_pos, "a constructor needs a block body");
    }
    else if (m.kind == Member::IsField || m.kind == Member::IsProp)
    {
        if (m.kind == Member::IsField)
        {
            m.field.access = mods.access;
            m.static_pos = mods.static_pos;
        }
        else
        {
            m.prop.is_static = mods.is_static;
            // An accessor without its own access word has the access of the property.
            m.prop.access = mods.access;
            if (!m.get_has_access)
                m.prop.get_access = mods.access;
            if (!m.set_has_access)
                m.prop.set_access = mods.access;
        }
        for (const ModTok& t : mods.kind_toks)
            st.error(t.pos,
                t.word == "abstract"
                    ? std::string(m.kind == Member::IsField ? "a field cannot be 'abstract'; only a method can be"
                                                            : "a property cannot be 'abstract'; only a method can be")
                    : "'" + t.word + "' is only valid on a method");
    }
    else
    {
        m.method.access = mods.access;
        m.method.is_override = mods.is_override;
        m.method.is_virtual = mods.is_virtual;
        m.method.is_new = mods.is_new;
        m.method.is_async = mods.is_async;
        m.method.is_static = mods.is_static;
        m.static_pos = mods.static_pos;
        if (mods.has_access)
            m.access_pos = mods.access_pos;
        for (const ModTok& t : mods.kind_toks)
            if (t.word == "async")
                m.async_pos = t.pos;
            else
                m.kind_words.emplace_back(t.pos, t.word);
        if (is_op)
        {
            // An operator has no modifier of the kind. The word is an error, the flags stay false.
            for (const ModTok& t : mods.kind_toks)
                st.error(t.pos, "an operator cannot be '" + t.word + "'");
            m.method.is_override = m.method.is_virtual = m.method.is_new = m.method.is_async = false;
            if (!mods.is_abstract && m.no_body_pos)
                st.error(m.no_body_pos,
                    "operator '" + m.method.op + "' needs a body; write '{ ... }'");
        }
        else if (mods.is_static)
        {
            // A static method has no object: it cannot be virtual, abstract or async.
            m.method.is_virtual = m.method.is_override = m.method.is_new = m.method.is_async = false;
            for (const ModTok& t : mods.kind_toks)
                if (t.word == "async")
                    st.error(t.pos, "an async function needs an object; remove 'static'");
                else
                    st.error(t.pos, "'static' and '" + t.word + "' cannot combine");
            if (m.no_body_pos && !mods.is_abstract)
                st.error(m.no_body_pos,
                    "method '" + m.method.name + "' needs a body; write '{ ... }'");
        }
        else if (mods.is_abstract)
        {
            m.method.is_abstract = true;
            for (const ModTok& t : mods.kind_toks)
                if (t.word == "virtual" || t.word == "new" || t.word == "override")
                    st.error(t.pos,
                        "'abstract' and '" + t.word + "' cannot combine"
                            + (t.word == "virtual" ? "; an abstract method is virtual already" : ""));
            if (m.no_body_pos == nullptr)
                st.error(mods.abstract_pos,
                    "an abstract method cannot have a body; end the declaration with ';'");
        }
        else if (m.no_body_pos)
            st.error(m.no_body_pos,
                "method '" + m.method.name + "' needs a body; write '{ ... }' or mark the method 'abstract'");
    }
    return std::move(m);
}

// ---------------------------------------------------------------- tokens
// An identifier. The Tag is the error text when there is none.
template <typename Tag> struct name_p : lexy::token_production
{
    static constexpr auto rule = dsl::must(id).error<Tag>;
    static constexpr auto value = lexy::as_string<std::string>;
};

// A string literal. The value is the raw text between the quotes.
struct string_value : lexy::token_production
{
    static constexpr auto rule = string_lit;
    static constexpr auto value =
        lexy::callback<std::string>([](const char* b, const char* e) { return std::string(b, e); });
};

// "<" ... ">" on one line. The value keeps the angle brackets and drops the white space.
struct angle_value : lexy::token_production
{
    static constexpr auto rule = dsl::lit_c<'<'> >> (dsl::position
                                     + dsl::loop(dsl::peek(dsl::lit_c<'>'>) >> dsl::break_
                                         | dsl::peek(dsl::newline) >> dsl::error<e_import_close>
                                         | dsl::eof >> dsl::error<e_import_close> | dsl::else_ >> any_unit)
                                     + dsl::position + dsl::lit_c<'>'>);
    static constexpr auto value = lexy::callback<std::string>([](const char* b, const char* e)
    {
        std::string path = "<";
        for (; b != e; ++b)
            if (!std::isspace(static_cast<unsigned char>(*b)))
                path += *b;
        return path + ">";
    });
};

// ---------------------------------------------------------------- types
struct ret_term
{
    static constexpr auto term = dsl::lit_c<'{'>;
};
struct field_term
{
    static constexpr auto term = dsl::lit_c<'='> / dsl::lit_c<';'> / dsl::lit_c<'}'>;
};
struct init_cfg
{
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = dsl::lit_c<';'> / dsl::lit_c<')'> / dsl::lit_c<']'> / dsl::lit_c<'}'>;
};

// ---------------------------------------------------------------- using
// The three forms of "using": an import ("using \"file.h\";" or "using <file>;"), a namespace
// ("using ns.sub;") and a type alias ("using Name = type;").
struct alias_term
{
    static constexpr auto term = dsl::lit_c<';'>;
};

// A name with optional "." parts. The text joins the parts with "::" (a "std" part is the std placeholder).
struct using_name_p
{
    static constexpr auto rule = dsl::list(dsl::p<name_p<e_import_name>>, dsl::sep(dsl::lit_c<'.'>));
    static constexpr auto value =
        lexy::fold_inplace<std::string>("", [](std::string& acc, std::string&& part)
    {
        if (!acc.empty())
            append_scope(acc);
        acc += part;
    });
};

// The part of a namespace or an alias after "using": the name and the optional "= type".
struct UsingRest
{
    const char* pos = nullptr;
    std::string name;
    bool has_type = false;
    const char* type_pos = nullptr;
    Span type;
};

struct using_rest_p
{
    static constexpr auto rule = dsl::position + dsl::p<using_name_p>
        + dsl::opt(dsl::lit_c<'='> >> (dsl::position + dsl::p<scan_type<alias_term>>));
    static constexpr auto value = lexy::callback<UsingRest>(
        [](const char* pos, std::string&& name, lexy::nullopt)
    {
        UsingRest r;
        r.pos = pos;
        r.name = std::move(name);
        return r;
    },
        [](const char* pos, std::string&& name, const char* type_pos, Span type)
    {
        UsingRest r;
        r.pos = pos;
        r.name = std::move(name);
        r.has_type = true;
        r.type_pos = type_pos;
        r.type = type;
        return r;
    });
};

struct import_p
{
    static constexpr auto rule = dsl::position + kw_using
        + (dsl::eof >> dsl::error<e_import_name> | dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_value>
            | dsl::peek(dsl::lit_c<'<'>) >> dsl::p<angle_value>
            | dsl::peek(id) >> dsl::p<using_rest_p> | dsl::else_ >> dsl::error<e_import_arg>)+dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(
        lexy::callback<TopItem>(
            [](const ParseState& st, const char* pos, std::string path)
    {
        if (path.size() > 2 && path.front() == '<' && path.back() != '>')
            st.error(pos, "using <...> needs a closing '>'");
        else if (path == "<>" || path.empty())
            st.error(pos, "using needs a header name");
        TopItem t;
        t.kind = TopItem::IsImport;
        ast::Import& im = t.import;
        im.path = std::move(path);
        im.pos = st.offset(pos);
        im.line = st.map->line(im.pos);
        return t;
    },
            [](const ParseState& st, const char* pos, UsingRest&& r)
    {
        TopItem t;
        const uint32_t at = st.offset(pos);
        if (!r.has_type)
        {
            t.kind = TopItem::IsUsingNs;
            t.using_ns.name = std::move(r.name);
            t.using_ns.pos = at;
            t.using_ns.line = st.map->line(at);
            return t;
        }
        t.kind = TopItem::IsAlias;
        if (r.name.find(':') != std::string::npos)
            st.error(r.pos, "a type alias has a plain name");
        t.alias.name = std::move(r.name);
        t.alias.type = type_of(st, r.type);
        t.alias.pos = at;
        t.alias.line = st.map->line(at);
        return t;
    }),
        lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- members
// A bad argument of an attribute. It reports one error and skips to the next "," or ")".
// The value is an empty string. The argument count stays right, so no second error follows.
struct bad_attr_arg
{
    static constexpr auto rule = dsl::position
        + dsl::loop(dsl::peek(dsl::lit_c<','> / dsl::lit_c<')'>) >> dsl::break_ | dsl::eof >> dsl::break_
            | dsl::else_ >> any_unit);
    static constexpr auto value = lexy::bind(lexy::callback<std::string>(
                                                 [](const ParseState& st, const char* pos)
    {
        st.error(pos, e_alias_string::name);
        return std::string();
    }),
        lexy::parse_state, lexy::values);
};

// The strings of an attribute: "(" string { "," string } ")".
struct attr_args
{
    static constexpr auto rule = dsl::lit_c<'('>
        + dsl::list(dsl::peek(dsl::lit_c<'"'>) >> dsl::p<string_value> | dsl::else_ >> dsl::p<bad_attr_arg>,
            dsl::sep(dsl::lit_c<','>))
        + dsl::lit_c<')'>;
    static constexpr auto value = lexy::as_list<std::vector<std::string>>;
};

// One attribute in the list: a name and optional strings in parentheses.
struct attr_p
{
    static constexpr auto rule =
        dsl::position + dsl::p<name_p<e_attr_name>> + dsl::opt(dsl::peek(dsl::lit_c<'('>) >> dsl::p<attr_args>);
    static constexpr auto value = lexy::callback<AttrItem>([](const char* pos, std::string name, lexy::nullopt)
    { return AttrItem{pos, std::move(name), {}}; },
        [](const char* pos, std::string name, std::vector<std::string>&& args)
    { return AttrItem{pos, std::move(name), std::move(args)}; });
};

struct attr_list
{
    static constexpr auto rule = dsl::terminator(dsl::lit_c<']'>).list(dsl::p<attr_p>, dsl::sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<AttrItem>>;
};

// "[" attribute { "," attribute } "]". Only "alias", "weak" and "byval" are known. "value" is
// removed: it gives an error.
struct attrs_p
{
    static constexpr auto rule = dsl::position + dsl::lit_c<'['> + dsl::p<attr_list>;
    static constexpr auto value =
        lexy::bind(lexy::callback<Annot>(
                       [](const ParseState& st, const char* pos, std::vector<AttrItem>&& items)
    {
        Annot a;
        a.pos = pos;
        for (AttrItem& it : items)
        {
            if (it.name == "weak")
            {
                if (!it.args.empty())
                    st.error(it.pos, "attribute 'weak' takes 0 arguments");
                else if (a.weak_pos)
                    st.error(it.pos, "duplicate attribute 'weak'");
                else
                    a.weak_pos = it.pos;
            }
            else if (it.name == "byval")
            {
                if (!it.args.empty())
                    st.error(it.pos, "attribute 'byval' takes 0 arguments");
                else if (a.byval_pos)
                    st.error(it.pos, "duplicate attribute 'byval'");
                else
                    a.byval_pos = it.pos;
            }
            else if (it.name == "value")
                st.error(it.pos, kValueRemoved);
            else if (it.name != "alias")
            {
                bool duplicate = false;
                for (const Annot::Tag& t : a.tags)
                    duplicate = duplicate || t.name == it.name;
                if (duplicate)
                    st.error(it.pos, "duplicate attribute '" + it.name + "'");
                else
                    a.tags.push_back({it.pos, std::move(it.name), std::move(it.args)});
            }
            else if (it.args.size() != 1)
                st.error(it.pos, "attribute 'alias' takes 1 arguments");
            else if (a.has_alias)
                st.error(it.pos, "duplicate attribute 'alias'");
            else
            {
                a.has_alias = true;
                a.alias = std::move(it.args[0]);
            }
        }
        return a;
    }),
            lexy::parse_state, lexy::values);
};

struct paren_cfg
{
    static constexpr auto open = dsl::lit_c<'('>;
    static constexpr auto term = dsl::lit_c<')'>;
};

// The end of a default value: a "," or a ")" at depth zero.
struct param_default_cfg
{
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = dsl::lit_c<','> / dsl::lit_c<')'>;
};

// "Type name" or "Type name = default".
struct param_p
{
    static constexpr auto rule = dsl::opt(dsl::peek(dsl::lit_c<'['>) >> dsl::p<attrs_p>) + dsl::position
        + param_mode_rule + dsl::p<scan_param_type> + dsl::p<name_p<e_param_name>>
        + dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<param_default_cfg>>) + dsl::if_(dsl::lit_c<','>);
    static constexpr auto value =
        lexy::bind(lexy::callback<ast::Param>(
                       [](const ParseState& st, const auto& ann, const char* pos, const auto& word, Span type,
                           std::string name, auto def)
    {
        check_name(st, pos, name);
        ast::Param p{std::move(name), ""};
        ast::ParamMode mode = param_mode_of(word);
        if (const Annot* a = annot_of(ann))
        {
            if (a->has_alias)
                st.error(a->pos, "[alias] applies to fields only");
            reject_member_tags(st, *a);
            p.weak = a->weak_pos != nullptr;
            if (a->byval_pos)
            {
                if (mode != ast::ParamMode::Plain)
                    st.error(a->byval_pos, "[byval] cannot be used with 'in', 'ref' or 'out'");
                mode = ast::ParamMode::ByVal;
            }
        }
        if (only_blank(span_text(st, type)))
            st.error(pos, "a parameter needs a type; write 'Type name'");
        else
            p.type = type_of(st, type);
        apply_param_mode(p, mode);
        if constexpr (!std::is_same_v<decltype(def), lexy::nullopt>)
        {
            p.def = std::make_shared<ast::ParamDefault>();
            p.def->span = def;
            p.def->pos = def.begin - 1;
            if (only_blank(span_text(st, def)))
                st.error(st.base + def.begin, "expected a default value");
        }
        return p;
    }),
            lexy::parse_state, lexy::values);
};

struct params_p
{
    static constexpr auto rule = dsl::lit_c<'('> + dsl::terminator(dsl::lit_c<')'>).opt_list(dsl::p<param_p>);
    static constexpr auto value = lexy::as_list<std::vector<ast::Param>>;
};

// The part of a method after the name: params, an optional base call and body.
constexpr auto kw_base = LEXY_KEYWORD("base", id);
constexpr const char* kConstInferred = "const is inferred; remove it";
constexpr auto kw_const_mark = LEXY_KEYWORD("const", id);

// "static_assert(cond, "message");" at file level or in a class body (gap G08). The arguments
// stay a raw span. The span-then-call step parses them.
constexpr auto kw_static_assert = LEXY_KEYWORD("static_assert", id);
struct static_assert_p
{
    static constexpr auto rule = dsl::position + kw_static_assert + dsl::p<scan_until<paren_cfg>> + dsl::lit_c<')'>
        + dsl::lit_c<';'>;
    static constexpr auto value = lexy::bind(lexy::callback<ast::StaticAssert>([](const ParseState& st, const char* pos, Span args)
    {
        ast::StaticAssert s;
        s.pos = st.offset(pos);
        s.line = st.map->line(s.pos);
        s.args_span = args;
        return s;
    }),
        lexy::parse_state, lexy::values);
};
struct static_assert_member_p
{
    static constexpr auto rule = dsl::p<static_assert_p>;
    static constexpr auto value = lexy::callback<Member>([](ast::StaticAssert&& s)
    {
        Member m;
        m.kind = Member::IsStaticAssert;
        m.sa = std::move(s);
        return m;
    });
};
struct static_assert_top_p
{
    static constexpr auto rule = dsl::p<static_assert_p>;
    static constexpr auto value = lexy::callback<TopItem>([](ast::StaticAssert&& s)
    {
        TopItem t;
        t.kind = TopItem::IsStaticAssert;
        t.sa = std::move(s);
        return t;
    });
};

// ": base(args)". The args stay a raw span. The call is valid in a constructor only.
struct base_call_p
{
    static constexpr auto rule = dsl::lit_c<':'>
        + dsl::position + (kw_base | dsl::else_ >> dsl::error<e_base>)+dsl::p<scan_until<paren_cfg>> + dsl::lit_c<')'>;
    static constexpr auto value =
        lexy::callback<BaseCall>([](const char* pos, Span args) { return BaseCall{pos, args}; });
};

// "=> e;" is the body of a method or of a property getter. The value is the span of "e".
struct arrow_cfg
{
    static constexpr auto open = LEXY_LIT("=>");
    static constexpr auto term = dsl::lit_c<';'> / dsl::lit_c<'}'>;
};

struct ArrowBody
{
    Span span;
};

struct arrow_body_p
{
    static constexpr auto rule = dsl::p<scan_until<arrow_cfg>> + dsl::lit_c<';'>;
    static constexpr auto value = lexy::callback<ArrowBody>([](Span s) { return ArrowBody{s}; });
};

// The optional "const" after the parameters of a method. The parser reads it to give one error:
// the script never writes "const" on a method (the const inference does not change a method).
struct const_mark_p
{
    static constexpr auto rule = dsl::capture(kw_const_mark);
    static constexpr auto value = lexy::callback<bool>([](auto) { return true; });
};

// ";" in place of the body: an abstract method. Other kinds of member report it.
struct NoBody
{
    const char* pos = nullptr;
};

struct no_body_p
{
    static constexpr auto rule = dsl::position + dsl::lit_c<';'>;
    static constexpr auto value = lexy::callback<NoBody>([](const char* pos) { return NoBody{pos}; });
};

struct method_tail
{
    static constexpr auto rule = dsl::p<params_p> + dsl::opt(dsl::peek(kw_const_mark) >> dsl::p<const_mark_p>)
        + dsl::opt(dsl::peek(dsl::lit_c<':'>) >> dsl::p<base_call_p>)
        + (dsl::peek(LEXY_LIT("=>")) >> dsl::p<arrow_body_p> | dsl::peek(dsl::lit_c<';'>) >> dsl::p<no_body_p>
            | dsl::else_ >> dsl::p<scan_balanced>);
    static constexpr auto value = lexy::bind(lexy::callback<MethodRest>(
                                                 [](const ParseState&, auto&& params, auto&& cm, auto&& sup, auto body_in)
    {
        MethodRest r;
        if constexpr (std::is_same_v<std::decay_t<decltype(cm)>, bool>)
            r.is_const = cm;
        Span body;
        if constexpr (std::is_same_v<decltype(body_in), NoBody>)
        {
            r.no_body_pos = body_in.pos;
        }
        else if constexpr (std::is_same_v<decltype(body_in), ArrowBody>)
        {
            body = body_in.span;
            r.expr_body = true;
        }
        else
        {
            body = body_in;
        }
        if constexpr (!std::is_same_v<std::decay_t<decltype(params)>, lexy::nullopt>)
            r.params = std::move(params);
        if constexpr (!std::is_same_v<std::decay_t<decltype(sup)>, lexy::nullopt>)
        {
            r.base_pos = sup.pos;
            r.base_args = sup.args;
        }
        r.body = body;
        return r;
    }),
        lexy::parse_state, lexy::values);
};

// The part of a field after the name: an optional initializer and the ";".
struct field_tail
{
    static constexpr auto rule = dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<init_cfg>>) + dsl::lit_c<';'>;
    static constexpr auto value = lexy::callback<FieldRest>([](auto init)
    {
        FieldRest f;
        if constexpr (!std::is_same_v<decltype(init), lexy::nullopt>)
        {
            f.has_init = true;
            f.init = init;
        }
        return f;
    });
};

// One accessor of a property: [access] ("get" | "set") (";" | "{ body }").
constexpr auto access_set = dsl::literal_set(kw_public, kw_private, kw_protected);

struct mod_item_access : lexy::token_production
{
    static constexpr auto rule = dsl::capture(access_set);
    static constexpr auto value =
        lexy::callback<ModTok>([](auto lex) { return ModTok{lex.begin(), std::string(lex.begin(), lex.end())}; });
};

struct accessor_p
{
    static constexpr auto rule = dsl::opt(dsl::peek(access_set) >> dsl::p<mod_item_access>) + dsl::position
        + dsl::must(dsl::literal_set(kw_get, kw_set)).error<e_accessor>
        + (dsl::peek(dsl::lit_c<'{'>) >> dsl::p<scan_balanced> | dsl::else_ >> dsl::lit_c<';'>);
    static constexpr auto value = lexy::callback<AccessorItem>([](auto&& acc, const char* pos, auto&&... body)
    {
        AccessorItem a;
        a.pos = pos;
        a.is_get = *pos == 'g';
        if constexpr (!std::is_same_v<std::decay_t<decltype(acc)>, lexy::nullopt>)
        {
            a.has_access = true;
            a.access = acc.word == "public" ? ast::Access::Public
                : acc.word == "private"     ? ast::Access::Private
                                            : ast::Access::Protected;
        }
        if constexpr (sizeof...(body) == 1)
        {
            a.has_body = true;
            ((a.body = body), ...);
        }
        return a;
    });
};

struct accessors_p
{
    static constexpr auto rule = dsl::lit_c<'{'> + dsl::terminator(dsl::lit_c<'}'>).opt_list(dsl::p<accessor_p>);
    static constexpr auto value = lexy::as_list<std::vector<AccessorItem>>;
};

// The part of a property after the name: the accessors and an optional "= init;".
struct prop_tail
{
    static constexpr auto rule = dsl::position + dsl::p<accessors_p>
        + dsl::opt(dsl::peek(dsl::lit_c<'='>) >> (dsl::p<scan_until<init_cfg>> + dsl::lit_c<';'>));
    static constexpr auto value = lexy::callback<PropRest>([](const char* open, auto&& items, auto&& init)
    {
        PropRest r;
        r.open = open;
        if constexpr (!std::is_same_v<std::decay_t<decltype(items)>, lexy::nullopt>)
            r.accessors = std::move(items);
        if constexpr (!std::is_same_v<std::decay_t<decltype(init)>, lexy::nullopt>)
        {
            r.has_init = true;
            r.init = init;
        }
        return r;
    });
};

// "Type Name => e;" is a read-only property. The part after the name.
struct arrow_prop_tail
{
    static constexpr auto rule = dsl::position + dsl::p<arrow_body_p>;
    static constexpr auto value = lexy::callback<PropRest>([](const char* open, ArrowBody b)
    {
        PropRest r;
        r.open = open;
        r.arrow = true;
        r.arrow_body = b.span;
        return r;
    });
};

// The word "operator" after the type of a member. The word starts an operator member.
constexpr auto kw_operator = LEXY_KEYWORD("operator", id);

// The symbols of an operator member. The literal set takes the longest match, so "<=>" wins over "<=".
// The checker accepts only some of them. The parser accepts all of them to give a clear error later.
constexpr auto op_symbol_set = dsl::literal_set(LEXY_LIT("<=>"), LEXY_LIT("<<="), LEXY_LIT(">>="), LEXY_LIT("[]"),
    LEXY_LIT("()"), LEXY_LIT("<<"), LEXY_LIT(">>"), LEXY_LIT("<="), LEXY_LIT(">="), LEXY_LIT("=="), LEXY_LIT("!="),
    LEXY_LIT("&&"), LEXY_LIT("||"), LEXY_LIT("++"), LEXY_LIT("--"), LEXY_LIT("+="), LEXY_LIT("-="), LEXY_LIT("*="),
    LEXY_LIT("/="), LEXY_LIT("%="), LEXY_LIT("&="), LEXY_LIT("|="), LEXY_LIT("^="), LEXY_LIT("->"), dsl::lit_c<'+'>,
    dsl::lit_c<'-'>, dsl::lit_c<'*'>, dsl::lit_c<'/'>, dsl::lit_c<'%'>, dsl::lit_c<'&'>, dsl::lit_c<'|'>,
    dsl::lit_c<'^'>, dsl::lit_c<'!'>, dsl::lit_c<'~'>, dsl::lit_c<'<'>, dsl::lit_c<'>'>, dsl::lit_c<'='>);

// "operator" and one symbol, then "(". The value is "operator" and the symbol, as one name. The
// callback of the member splits it again (see split_operator_name).
struct operator_name_p
{
    static constexpr auto rule = kw_operator
        + dsl::must(dsl::capture(op_symbol_set)).error<e_op_symbol>
        + dsl::must(dsl::peek(dsl::lit_c<'('>)).error<e_op_params>;
    static constexpr auto value = lexy::callback<std::string>(
        [](auto sym) { return "operator" + std::string(sym.begin(), sym.end()); });
};

// The name of a member: an identifier, or "operator" and a symbol.
struct member_name_p
{
    static constexpr auto rule = dsl::peek(kw_operator) >> dsl::p<operator_name_p> | dsl::else_ >> dsl::p<name_p<e_member_name>>;
    static constexpr auto value = lexy::forward<std::string>;
};

// True when the name comes from operator_name_p: "operator" and a character that is not a word character.
bool is_operator_name(const std::string& name)
{
    return name.size() > 8 && name.compare(0, 8, "operator") == 0
        && !(std::isalnum(static_cast<unsigned char>(name[8])) || name[8] == '_' || name[8] == '$');
}

// The name of a member: an identifier before "(", "=", ";" or "{" (a property).
constexpr auto member_name_ahead =
    id + dsl::whitespace(ws_rule) + (dsl::lit_c<'('> / dsl::lit_c<'='> / dsl::lit_c<';'> / dsl::lit_c<'{'>);

// Matches the type of a member. The scan stops before the name.
// The name is the last identifier before "(", "=", ";" or "{" at depth zero.
// An identifier after a "." is part of the type, so "std.string" is a type and not a name.
// The span is empty when the member has no type.
struct scan_member_type : lexy::token_production
{
    static constexpr auto rule = dsl::position
        + dsl::loop(dsl::peek(kw_operator) >> dsl::break_ | dsl::peek(member_name_ahead) >> dsl::break_
            | dsl::peek(
                  dsl::literal_set(dsl::lit_c<'('>, dsl::lit_c<'='>, dsl::lit_c<';'>, dsl::lit_c<'{'>, dsl::lit_c<'}'>))
                >> dsl::break_
            | type_ops | dsl::lit_c<'.'> >> dsl::if_(dsl::token(id)) | dsl::token(id)
            | dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> dsl::p<type_item>)
        + dsl::position;
    static constexpr auto value = span_value;
};

// "Type name(params) { body }" or "Type name = init;" or "Type name;".
struct typed_member
{
    static constexpr auto rule = dsl::position + dsl::p<scan_member_type>
        + dsl::position + dsl::p<member_name_p>
        + (dsl::peek(dsl::lit_c<'('>) >> dsl::p<method_tail> | dsl::peek(dsl::lit_c<'{'>) >> dsl::p<prop_tail>
            | dsl::peek(LEXY_LIT("=>")) >> dsl::p<arrow_prop_tail> | dsl::else_ >> dsl::p<field_tail>);
    static constexpr auto value = lexy::bind(lexy::callback<Member>(
                                                 [](const ParseState& st, const char* start, Span type,
                                                     const char* name_pos, std::string name, PropRest&& rest)
    {
        Member m;
        m.kind = Member::IsProp;
        ast::Property& p = m.prop;
        check_name(st, name_pos, name);
        if (only_blank(span_text(st, type)))
            st.error(start, "a property needs a type; write 'Type Name { get; set; }'");
        p.name = std::move(name);
        p.type = type_of(st, type, false);
        for (const AccessorItem& a : rest.accessors)
        {
            const char* word = a.is_get ? "get" : "set";
            if (a.is_get ? p.has_get : p.has_set)
            {
                st.error(a.pos, std::string("duplicate '") + word + "' accessor");
                continue;
            }
            (a.is_get ? p.has_get : p.has_set) = true;
            (a.is_get ? p.get_pos : p.set_pos) = st.offset(a.pos);
            (a.is_get ? m.get_has_access : m.set_has_access) = a.has_access;
            (a.is_get ? p.get_access : p.set_access) = a.access;
            if (a.has_body)
            {
                (a.is_get ? p.get_body : p.set_body).emplace();
                (a.is_get ? p.get_span : p.set_span) = a.body;
            }
        }
        if (rest.arrow)
        {
            p.has_get = true;
            p.get_is_expr = true;
            p.get_pos = st.offset(name_pos);
            p.get_body.emplace();
            p.get_span = rest.arrow_body;
            if (only_blank(span_text(st, rest.arrow_body)))
                st.error(st.base + rest.arrow_body.begin, "expected an expression");
        }
        if (!p.has_get && !p.has_set)
            st.error(rest.open, "a property needs a 'get' or a 'set' accessor");
        const bool custom_get = p.get_body.has_value(), custom_set = p.set_body.has_value();
        if (p.has_get && p.has_set && custom_get != custom_set)
            st.error(rest.open, "a property cannot mix auto and custom accessors");
        if (rest.has_init)
        {
            p.has_init = true;
            p.init_span = rest.init;
            if (!p.is_auto())
                st.error(st.base + rest.init.begin - 1, "only an auto property can have an initializer");
            else if (only_blank(span_text(st, rest.init)))
                st.error(st.base + rest.init.begin, "expected an initializer");
        }
        p.pos = st.offset(name_pos);
        p.line = st.map->line(p.pos);
        return m;
    },
                                                 [](const ParseState& st, const char* start, Span type,
                                                     const char* name_pos, std::string name, MethodRest&& rest)
    {
        Member m;
        const bool is_op = is_operator_name(name);
        if (!is_op)
            check_name(st, name_pos, name);
        if (only_blank(span_text(st, type)))
        {
            // No type: a constructor, when the name is the name of the class. The step
            // that adds the members to the class checks the name.
            m.kind = Member::IsCtor;
            if (rest.expr_body)
                st.error(name_pos, "a constructor needs a block body; '=>' is for methods");
            if (rest.is_const)
                st.error(name_pos, "a constructor cannot be 'const'");
            ast::Ctor& c = m.ctor;
            m.ctor_name = std::move(name);
            c.params = std::move(rest.params);
            c.body_span = rest.body;
            m.no_body_pos = rest.no_body_pos;
            c.pos = st.offset(name_pos);
            c.line = st.map->line(c.pos);
            c.end_pos = rest.no_body_pos ? st.offset(rest.no_body_pos) : rest.body.end;
            c.end_line = st.map->line(c.end_pos);
            if (rest.base_pos)
            {
                c.has_base_call = true;
                c.base_span = rest.base_args;
                c.base_pos = st.offset(rest.base_pos);
            }
            return m;
        }
        m.kind = Member::IsMethod;
        ast::Method& me = m.method;
        std::string ret = type_of(st, type);
        if (rest.base_pos)
            st.error(rest.base_pos, "only a constructor can call 'base(...)' after the parameters");
        me.params = std::move(rest.params);
        if (is_op)
        {
            // The identifier of the operator. An unknown symbol keeps "operator" and the symbol.
            me.op = name.substr(8);
            const std::optional<std::string> ident = op_ident(me.op, me.params.size());
            me.name = ident ? *ident : std::move(name);
        }
        else
        {
            me.name = std::move(name);
        }
        me.ret = ret == "void" ? std::string{} : std::move(ret);
        me.body_span = rest.body;
        me.expr_body = rest.expr_body;
        if (rest.is_const)
            st.error(name_pos, kConstInferred);
        m.no_body_pos = rest.no_body_pos;
        if (rest.expr_body && only_blank(span_text(st, rest.body)))
            st.error(st.base + rest.body.begin, "expected an expression");
        me.pos = st.offset(start);
        me.line = st.map->line(me.pos);
        me.end_pos = rest.no_body_pos ? st.offset(rest.no_body_pos) : rest.body.end;
        me.end_line = st.map->line(me.end_pos);
        return m;
    },
                                                 [](const ParseState& st, const char* start, Span type,
                                                     const char* name_pos, std::string name, FieldRest&& rest)
    {
        Member m;
        m.kind = Member::IsField;
        ast::Field& f = m.field;
        check_name(st, name_pos, name);
        if (only_blank(span_text(st, type)))
            st.error(start, "a field needs a type; write 'Type name;'");
        f.name = std::move(name);
        f.type = type_of(st, type, false);
        if (f.type.compare(0, 6, "const ") == 0)
            st.error(start, kConstInferred);
        if (rest.has_init)
        {
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

// C++ words that a script member cannot have. The parser accepts them as modifiers to give a clear error.
constexpr auto kw_static = LEXY_KEYWORD("static", id);
constexpr auto kw_thread_local = LEXY_KEYWORD("thread_local", id);
constexpr auto kw_friend = LEXY_KEYWORD("friend", id);
constexpr auto kw_mutable = LEXY_KEYWORD("mutable", id);
constexpr auto kw_volatile = LEXY_KEYWORD("volatile", id);

constexpr auto modifier_set = dsl::literal_set(kw_override, kw_virtual, kw_abstract, kw_async, kw_new, kw_public, kw_private,
    kw_protected, kw_static, kw_thread_local, kw_friend, kw_mutable, kw_volatile, kw_const_mark);

struct mod_item : lexy::token_production
{
    static constexpr auto rule = dsl::position + dsl::capture(modifier_set);
    static constexpr auto value = lexy::callback<ModTok>(
        [](const char* pos, auto lex) { return ModTok{pos, std::string(lex.begin(), lex.end())}; });
};

// One or more modifiers, in any order.
struct mods_p
{
    static constexpr auto rule = dsl::list(dsl::peek(modifier_set) >> dsl::p<mod_item>);
    static constexpr auto value = lexy::as_list<std::vector<ModTok>>;
};

// "~Name() { body }". The add step checks the name.
struct dtor_p
{
    static constexpr auto rule =
        dsl::position + dsl::lit_c<'~'> + dsl::position + dsl::p<name_p<e_member_name>> + dsl::p<method_tail>;
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>(
            [](const ParseState& st, const char* start, const char* name_pos, std::string name, MethodRest&& rest)
    {
        Member m;
        m.kind = Member::IsDtor;
        if (rest.expr_body)
            st.error(name_pos, "a destructor needs a block body; '=>' is for methods");
        if (rest.is_const)
            st.error(name_pos, "a destructor cannot be 'const'");
        ast::Method& me = m.method;
        if (!rest.params.empty())
            st.error(name_pos, "a destructor has no parameters");
        if (rest.base_pos)
            st.error(rest.base_pos, "only a constructor can call 'base(...)'");
        m.ctor_name = name;
        me.name = "~" + name;
        me.body_span = rest.body;
        m.no_body_pos = rest.no_body_pos;
        me.pos = st.offset(start);
        me.line = st.map->line(me.pos);
        me.end_pos = rest.no_body_pos ? st.offset(rest.no_body_pos) : rest.body.end;
        me.end_line = st.map->line(me.end_pos);
        return m;
    }),
        lexy::parse_state, lexy::values);
};

// "using Name = type;" in a class body. The other forms of "using" are not members.
struct using_member_p
{
    static constexpr auto rule = dsl::p<import_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>([](const ParseState& st, TopItem&& t)
    {
        Member m;
        if (t.kind == TopItem::IsAlias)
        {
            m.kind = Member::IsAlias;
            m.alias = std::move(t.alias);
            return m;
        }
        const uint32_t at = t.kind == TopItem::IsImport ? t.import.pos : t.using_ns.pos;
        st.error(st.base + at, "a class has only a type alias 'using Name = type;'");
        return m;
    }),
        lexy::parse_state, lexy::values);
};

struct member_p
{
    static constexpr auto rule = dsl::position + dsl::opt(dsl::peek(dsl::lit_c<'['>) >> dsl::p<attrs_p>)
        + dsl::opt(dsl::peek(modifier_set) >> dsl::p<mods_p>)
        + (dsl::eof >> dsl::error<e_unexpected_eof> | dsl::peek(dsl::lit_c<'~'>) >> dsl::p<dtor_p>
            | dsl::peek(kw_using) >> dsl::p<using_member_p>
            | dsl::peek(kw_static_assert) >> dsl::p<static_assert_member_p> | dsl::peek(id) >> dsl::p<typed_member>
            | dsl::peek(dsl::lit_c<'$'>) >> dsl::p<typed_member> | dsl::else_ >> dsl::error<e_member>(junk));
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>([](const ParseState& st, const char*, const auto& ann, const auto& mods, Member&& m)
    { return finish_member(st, ann, mods, std::move(m)); }),
        lexy::parse_state, lexy::values);
};

// Skips the rest of a broken member. It stops after a ";" or before a "}" or "override".
struct sync_member : lexy::token_production
{
    static constexpr auto rule =
        dsl::loop(dsl::eof >> dsl::break_ | // the broken member gave the error; the class keeps its members
            dsl::lit_c<';'> >> dsl::break_ | dsl::peek(dsl::literal_set(dsl::lit_c<'}'>, kw_override)) >> dsl::break_
            | dsl::else_ >> dsl::p<skip_item>);
    static constexpr auto value = lexy::noop;
};

// The members up to the "}". A missing "}" at the end of the file is one error ("expected
// '}'"). The class keeps the members that the parser read (a tolerant parse).
struct members_p
{
    static constexpr auto closer = dsl::eof | dsl::lit_c<'}'>;
    static constexpr auto item = dsl::peek_not(closer) >> dsl::try_(dsl::p<member_p>, dsl::p<sync_member>);
    static constexpr auto rule = dsl::opt(dsl::peek_not(closer) >> dsl::list(item)) + dsl::try_(dsl::lit_c<'}'>);
    static constexpr auto value = lexy::as_list<std::vector<Member>>;
};

// ---------------------------------------------------------------- classes
// The name of a base class with optional "." parts. The text joins the parts with "::".
struct base_name_p
{
    static constexpr auto rule = dsl::list(dsl::p<name_p<e_base_name>>, dsl::sep(dsl::lit_c<'.'>));
    static constexpr auto value =
        lexy::fold_inplace<std::string>("", [](std::string& acc, std::string&& part)
    {
        if (!acc.empty())
            append_scope(acc);
        acc += part;
    });
};

// The template argument list of a base name: "<int, std.string>". The value is the span.
struct base_args_p
{
    static constexpr auto rule = dsl::position + dsl::p<type_item> + dsl::position;
    static constexpr auto value = span_value;
};

// A base class name with optional "." parts and an optional template argument list
// ("Tmpl<int>"). A "::" in the name is an error.
struct base_p
{
    static constexpr auto rule = dsl::if_(dsl::peek(LEXY_LIT("::")) >> dsl::error<e_use_dot_colons>)
        + dsl::p<base_name_p> + dsl::opt(dsl::peek(dsl::lit_c<'<'>) >> dsl::p<base_args_p>)
        + dsl::if_(dsl::peek(LEXY_LIT("::")) >> dsl::error<e_use_dot_colons>);
    static constexpr auto value = lexy::bind(lexy::callback<std::string>([](const ParseState&, std::string&& name,
                                                                             lexy::nullopt) { return std::move(name); },
                                                 [](const ParseState& st, std::string&& name, Span args)
    {
        const std::string_view raw(st.base + args.begin, args.end - args.begin);
        std::size_t colons = std::string_view::npos;
        const std::string text = type_text(raw, &colons);
        if (colons != std::string_view::npos)
            st.error(st.base + args.begin + colons, e_use_dot_colons::name);
        return array_types(st, st.base + args.begin, canonical_type_at(st, st.base + args.begin, name + text));
    }),
        lexy::parse_state, lexy::values);
};

// The bases of a class: "Base" or "Base, IFace, ...". The first name is the primary base. The
// other names are native interface bases (design 4.4).
struct bases_p
{
    static constexpr auto rule = dsl::list(dsl::p<base_p>, dsl::sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<std::string>>;
};

// The attributes before a class or a stub. The parser checks the names and the argument counts.
struct ClassAttrs
{
    const char* pos = nullptr; // the "["
    bool is_native = false, is_value = false, is_self = false, is_checked_index = false, is_nonnull = false;
    std::string handle, default_handle, intrusive_add, intrusive_release;
    std::string weak_handle; // [weak_handle("template")]: valid on a stub only
    std::string factory; // [factory("template")]: valid on a stub only
    std::string cpp_prelude; // [cpp_prelude("text")]
    bool has_prelude = false;
    std::string cpp_overrides; // [cpp_overrides("A B")]: valid on a stub only
    std::string alias; // [alias("old")]: valid on a module variable only
    bool has_alias = false;
    std::string std_ns; // [std("name")]: valid in a module attribute list only
    bool has_std = false;
    const char* std_pos = nullptr; // the "std" name, or null
    // [lib("name")]: a standard-like library namespace. Valid in a module attribute list only. The
    // attribute can come more than one time, with one namespace each time.
    std::vector<std::pair<const char*, std::string>> libs;
    std::string array_tpl; // [array("template")]: valid in a module attribute list only
    bool has_array = false;
    const char* array_pos = nullptr; // the "array" name, or null
    const char* handle_pos = nullptr; // the "handle" name, or null
    bool is_attribute = false; // [attribute]: valid on "class Name;" only
    std::vector<std::pair<const char*, std::string>> used; // the valid attributes
    // The other names without arguments: script attribute tags. Only a script class accepts them.
    std::vector<std::pair<const char*, std::string>> tags;
};

// The attributes of a stub, a class and a module attribute list, with the count of arguments.
// hotc::class_attribute_names gives the names to the tools (hot-ls).
struct ClassAttr
{
    const char* name;
    int arity;
};
constexpr ClassAttr kClassAttrs[] = {{"native", 0}, {"value", 0}, {"self", 0}, {"checked_index", 0}, {"nonnull", 0}, {"weak", 0},
    {"attribute", 0}, {"handle", 1}, {"default_handle", 1}, {"weak_handle", 1}, {"factory", 1}, {"alias", 1}, {"cpp_prelude", 1},
    {"cpp_overrides", 1}, {"std", 1}, {"lib", 1}, {"array", 1}, {"intrusive", 2}};

// The expected count of arguments of a class attribute, or -1 for an unknown name.
int class_attr_arity(const std::string& name)
{
    for (const ClassAttr& a : kClassAttrs)
        if (name == a.name)
            return a.arity;
    return -1;
}

// "[" attribute { "," attribute } "]" before "class".
struct class_attrs_p
{
    static constexpr auto rule = dsl::position + dsl::lit_c<'['> + dsl::p<attr_list>;
    static constexpr auto value =
        lexy::bind(lexy::callback<ClassAttrs>(
                       [](const ParseState& st, const char* pos, std::vector<AttrItem>&& items)
    {
        ClassAttrs a;
        a.pos = pos;
        for (AttrItem& it : items)
        {
            const int arity = class_attr_arity(it.name);
            bool dup = false;
            for (const auto& u : a.used)
                dup = dup || (u.second == it.name && it.name != "lib"); // "lib" is repeatable
            for (const auto& u : a.tags)
                dup = dup || u.second == it.name;
            if (arity < 0 && it.args.empty() && !dup)
            {
                // A script attribute tag. The translator checks that an attribute class has the name.
                a.tags.emplace_back(it.pos, it.name);
                continue;
            }
            if (arity < 0 && !(dup && it.args.empty()))
            {
                st.error(it.pos, "unknown attribute '" + it.name + "'");
                continue;
            }
            if (arity < 0)
            {
                st.error(it.pos, "duplicate attribute '" + it.name + "'");
                continue;
            }
            if (static_cast<size_t>(arity) != it.args.size())
            {
                st.error(it.pos, "attribute '" + it.name + "' takes " + std::to_string(arity) + " arguments");
                continue;
            }
            if (dup)
            {
                st.error(it.pos, "duplicate attribute '" + it.name + "'");
                continue;
            }
            a.used.emplace_back(it.pos, it.name);
            if (it.name == "native")
                a.is_native = true;
            else if (it.name == "value")
                a.is_value = true;
            else if (it.name == "self")
                a.is_self = true;
            else if (it.name == "checked_index")
                a.is_checked_index = true;
            else if (it.name == "nonnull")
                a.is_nonnull = true;
            else if (it.name == "attribute")
                a.is_attribute = true;
            else if (it.name == "cpp_prelude")
            {
                a.cpp_prelude = std_from_user(it.args[0]);
                a.has_prelude = true;
            }
            else if (it.name == "cpp_overrides")
                a.cpp_overrides = std::move(it.args[0]);
            else if (it.name == "alias")
            {
                a.alias = std::move(it.args[0]);
                a.has_alias = true;
            }
            else if (it.name == "std")
            {
                a.std_ns = std::move(it.args[0]);
                a.has_std = true;
                a.std_pos = it.pos;
            }
            else if (it.name == "lib")
                a.libs.emplace_back(it.pos, std::move(it.args[0]));
            else if (it.name == "array")
            {
                a.array_tpl = std_from_user(it.args[0]);
                a.has_array = true;
                a.array_pos = it.pos;
            }
            else if (it.name == "handle")
            {
                a.handle = std_from_user(it.args[0]);
                a.handle_pos = it.pos;
            }
            else if (it.name == "default_handle")
                a.default_handle = std_from_user(it.args[0]);
            else if (it.name == "weak_handle")
                a.weak_handle = std_from_user(it.args[0]);
            else if (it.name == "factory")
                a.factory = std_from_user(it.args[0]);
            else if (it.name == "weak")
            {
                // A field, a parameter or a local takes "[weak]". The user of the list reports it.
            }
            else
            {
                a.intrusive_add = std_from_user(it.args[0]);
                a.intrusive_release = std_from_user(it.args[1]);
            }
        }
        return a;
    }),
            lexy::parse_state, lexy::values);
};

// The "*" in a stub name.
struct star_tok : lexy::token_production
{
    static constexpr auto rule = dsl::capture(dsl::lit_c<'*'>);
    static constexpr auto value = lexy::as_string<std::string>;
};

// The suffix "<*>" of a stub name.
struct star_suffix_tok : lexy::token_production
{
    static constexpr auto rule = dsl::capture(LEXY_LIT("<*>"));
    static constexpr auto value = lexy::as_string<std::string>;
};

// One part of a stub name: a name or "*".
struct stub_part
{
    static constexpr auto rule =
        dsl::peek(dsl::lit_c<'*'>) >> dsl::p<star_tok> | dsl::else_ >> dsl::p<name_p<e_class_name>>;
    static constexpr auto value = lexy::forward<std::string>;
};

// The parts of a stub name. The text joins the parts with "::".
struct stub_dotted
{
    static constexpr auto rule = dsl::list(dsl::p<stub_part>, dsl::sep(dsl::lit_c<'.'>));
    static constexpr auto value =
        lexy::fold_inplace<std::string>("", [](std::string& acc, std::string&& part)
    {
        if (!acc.empty())
            append_scope(acc);
        acc += part;
    });
};

// "*", or a dotted name with an optional suffix "<*>". A ".*" at the end becomes "::*".
struct stub_name_p
{
    static constexpr auto rule = dsl::p<stub_dotted> + dsl::opt(dsl::peek(LEXY_LIT("<*>")) >> dsl::p<star_suffix_tok>);
    static constexpr auto value = lexy::callback<std::string>([](std::string&& name, lexy::nullopt)
    { return std::move(name); }, [](std::string&& name, std::string&& suffix) { return std::move(name) + suffix; });
};

void add_members(const ParseState&, ast::ClassDef&, lexy::nullopt)
{
}
void add_members(const ParseState& st, ast::ClassDef& c, std::vector<Member>&& members)
{
    for (Member& m : members)
    {
        if (m.kind == Member::IsStaticAssert)
        {
            c.static_asserts.push_back(std::move(m.sa));
            continue;
        }
        if (m.kind == Member::IsDtor)
        {
            if (m.ctor_name == c.name)
                c.dtors.push_back(std::move(m.method));
            else
                st.error(st.base + m.method.pos, "the name of a destructor is '~" + c.name + "'");
        }
        else if (m.kind == Member::IsCtor)
        {
            if (m.ctor_name == c.name)
                c.ctors.push_back(std::move(m.ctor));
            else
                st.error(st.base + m.ctor.pos, "a method needs a return type; write 'Ret name(...)'");
        }
        else if (m.kind == Member::IsMethod)
        {
            if (m.method.is_abstract && !c.is_abstract)
                st.error(st.base + m.method.pos,
                    "abstract method '" + m.method.name + "' needs an abstract " + (c.is_value ? "struct" : "class")
                        + ": write 'abstract class " + c.name + "'");
            if (m.method.is_static)
                c.statics.push_back(std::move(m.method));
            else
                c.methods.push_back(std::move(m.method));
        }
        else if (m.kind == Member::IsField)
        {
            if (m.const_pos && !m.static_pos)
                st.error(m.const_pos,
                    "a class member cannot be 'const'; for a class constant, write 'static const' before the type of '" + m.field.name
                        + "'");
            else if (m.const_pos)
            {
                // A class constant. The parse step moves it to the module constants (Field::owner).
                if (!m.field.has_init)
                    st.error(m.const_pos, "the constant '" + m.field.name + "' needs a value");
                c.consts.push_back(std::move(m.field));
                continue;
            }
            if (m.static_pos)
            {
                // A static field (gap G23). The parse step moves it to the module variables.
                c.svars.push_back(std::move(m.field));
                continue;
            }
            c.fields.push_back(std::move(m.field));
        }
        else if (m.kind == Member::IsProp)
            c.props.push_back(std::move(m.prop));
        else if (m.kind == Member::IsAlias)
            c.aliases.push_back(std::move(m.alias));
    }
}

const ClassAttrs* class_attrs_of(lexy::nullopt)
{
    return nullptr;
}
const ClassAttrs* class_attrs_of(const ClassAttrs& a)
{
    return &a;
}

// A tag is valid only on a script class. Other places report it as an unknown attribute.
void reject_tags(const ParseState& st, const ClassAttrs* a)
{
    if (a)
        for (const auto& t : a->tags)
            st.error(t.first, "unknown attribute '" + t.second + "'");
}

// "[attribute] class Name;": a script attribute class. It takes no other attribute.
TopItem make_attribute_class(const ParseState& st, const char* pos, const ClassAttrs& a, bool is_struct,
    std::string&& name, bool derived)
{
    TopItem t;
    t.kind = TopItem::IsStub;
    ast::Stub& s = t.stub;
    s.name = std::move(name);
    s.is_attribute = true;
    s.pos = st.offset(pos);
    s.line = st.map->line(s.pos);
    reject_tags(st, &a);
    if (is_struct || derived || s.name.find_first_of("*<:.") != std::string::npos)
        st.error(pos, "an attribute class is declared as '[attribute] class Name;' with a plain name");
    for (const auto& u : a.used)
        if (u.second != "attribute")
            st.error(u.first, "attribute '" + u.second + "' is not allowed on an attribute class");
    return t;
}

// Build a stub from the attributes. "derived" means "class : Base;": the name is then the base.
TopItem make_stub(const ParseState& st, const char* pos, const ClassAttrs* a, bool is_struct, std::string&& name,
    bool derived)
{
    if (a && a->is_attribute)
        return make_attribute_class(st, pos, *a, is_struct, std::move(name), derived);
    reject_tags(st, a);
    TopItem t;
    t.kind = TopItem::IsStub;
    ast::Stub& s = t.stub;
    const bool star = name == "*";
    s.name = std::move(name);
    s.derived = derived;
    s.is_struct = is_struct;
    s.pos = st.offset(pos);
    s.line = st.map->line(s.pos);
    if (a)
    {
        s.is_native = a->is_native;
        s.is_value = a->is_value;
        s.is_self = a->is_self;
        s.checked_index = a->is_checked_index;
        s.nonnull = a->is_nonnull;
        s.handle = a->handle;
        s.default_handle = a->default_handle;
        s.weak_handle = a->weak_handle;
        s.factory = a->factory;
        s.intrusive_add = a->intrusive_add;
        s.intrusive_release = a->intrusive_release;
        s.cpp_prelude = a->cpp_prelude;
        s.cpp_overrides = a->cpp_overrides;
        for (const auto& u : a->used)
            s.attrs.push_back(u.second);
        if (a->is_value && a->handle_pos)
            st.error(a->handle_pos, "'value' and 'handle' are exclusive");
        if (a->has_alias)
            st.error(a->pos, "attribute 'alias' is not allowed on a stub");
        for (const auto& u : a->used)
            if (u.second == "weak")
                st.error(u.first, "attribute 'weak' is not allowed on a stub: put it on a field, a parameter or a local");
        if (a->has_std)
            st.error(a->std_pos, "attribute 'std' is valid only on a module attribute list: end the list with ';'");
        if (a->has_array)
            st.error(a->array_pos, "attribute 'array' is valid only on a module attribute list: end the list with ';'");
        for (const auto& l : a->libs)
            st.error(l.first, "attribute 'lib' is valid only on a module attribute list: end the list with ';'");
        if (a->is_self && (derived || s.name.find('*') != std::string::npos))
            st.error(a->pos, "'self' is valid only on a stub with an exact name");
        if (a->is_checked_index && !is_struct)
        {
            // The runtime trait needs a name or a template that C++ can write: "Name" or "Name<*>".
            const bool tpl = s.name.size() > 3 && s.name.compare(s.name.size() - 3, 3, "<*>") == 0;
            const std::string base = tpl ? s.name.substr(0, s.name.size() - 3) : s.name;
            if (derived)
                st.error(a->pos, "'checked_index' is not allowed on 'class : Base': name the type itself");
            else if (base.find('*') != std::string::npos)
                st.error(a->pos, "'checked_index' is valid only on an exact name or on 'Name<*>'");
        }
    }
    if (is_struct && (derived || !star))
        st.error(pos, "a native stub is declared with 'class', not 'struct'");
    else if (is_struct)
    {
        if (a)
            for (const auto& u : a->used)
                if (u.second != "cpp_prelude" && u.second != "native")
                    st.error(a->pos, "'struct *' accepts only 'cpp_prelude'");
    }
    else if (!s.is_native && !derived && !star)
        st.error(pos, "a stub needs the attribute 'native'");
    return t;
}

// The attributes of a module attribute list and the number of their arguments. A new attribute
// needs a row here, a field in ast::ModuleAttrs and a branch in make_module_attrs.
int module_attr_arity(const std::string& name)
{
    if (name == "std" || name == "lib" || name == "array")
        return 1;
    return -1;
}

// The end ";" of a module attribute list, or a bare ";" that does nothing. The value is the position of the ";".
struct module_attrs_end_p
{
    static constexpr auto rule = dsl::position + dsl::lit_c<';'>;
    static constexpr auto value =
        lexy::callback<ModuleAttrsMark>([](const char* pos) { return ModuleAttrsMark{pos}; });
};

TopItem make_module_attrs(const ParseState& st, const char* pos, const ClassAttrs* a)
{
    TopItem t;
    if (!a)
        return t; // a bare ";" does nothing
    t.kind = TopItem::IsModuleAttrs;
    ast::ModuleAttrs& m = t.module_attrs;
    m.pos = st.offset(pos);
    m.line = st.map->line(m.pos);
    for (const auto& u : a->used)
    {
        if (module_attr_arity(u.second) < 0)
            st.error(u.first, "attribute '" + u.second + "' is not allowed in a module attribute list");
        else if (u.second != "lib") // repeatable: not in the list of the names that come once
            m.used.emplace_back(u.second, st.offset(u.first));
    }
    for (const auto& [lpos, ns] : a->libs)
    {
        if (!valid_std_ns(ns))
            st.error(lpos, "'" + ns + "' is not a C++ namespace name");
        else if (ns == "std")
            st.error(lpos, "'lib(\"std\")' has no effect: the standard namespace is always a library namespace");
        else if (std::find(m.libs.begin(), m.libs.end(), ns) == m.libs.end())
            m.libs.push_back(ns);
    }
    if (a->has_std)
    {
        m.has_std = true;
        m.std_ns = a->std_ns;
        m.std_pos = st.offset(a->std_pos);
        if (!valid_std_ns(m.std_ns))
            st.error(a->std_pos, "'" + m.std_ns + "' is not a C++ namespace name");
    }
    if (a->has_array)
    {
        std::string why;
        if (valid_array_tpl(a->array_tpl, why))
        {
            m.has_array = true;
            m.array_tpl = a->array_tpl;
            m.array_pos = st.offset(a->array_pos);
        }
        else
            st.error(a->array_pos, why);
    }
    return t;
}

// The part of a class, a struct or a stub after the keyword: the name, then ";" (a stub) or
// the base and the members. The value is the position and the name first.
// "class : Base;" is a stub for the subclasses of Base. It gives one value: the base name.
constexpr auto class_rest = dsl::position
    + (dsl::peek(dsl::lit_c<':'>) >> (dsl::lit_c<':'> + dsl::p<base_p> + dsl::lit_c<';'>)
        | dsl::else_ >> (dsl::p<stub_name_p>
            + (dsl::lit_c<';'> >> dsl::nullopt
                | dsl::else_ >> (dsl::opt(dsl::lit_c<':'> >> dsl::p<bases_p>) + dsl::lit_c<'{'> + dsl::p<members_p>))));

// [attributes] class name ";" is a stub. [attributes] class name ":" Base "{" members "}" is a class.
// A "struct" is a script value type. Else the declaration is a module variable:
// [alias("old")] Type name [= init];
// "abstract" before "class" or "struct": the value is the position of the word, or nullopt.
constexpr auto abstract_prefix = dsl::opt(dsl::peek(kw_abstract) >> dsl::position + kw_abstract);
constexpr auto class_or_struct = dsl::literal_set(kw_class, kw_struct);

const char* abstract_pos_of(lexy::nullopt)
{
    return nullptr;
}
const char* abstract_pos_of(const char* p)
{
    return p;
}

// ---------------------------------------------------------------- interfaces
// One member of an interface body. The parser accepts the member forms of a class. It reports
// each attribute and each modifier here. The interface step reports the member kinds.
void iface_mods_error(const ParseState&, lexy::nullopt)
{
}
void iface_mods_error(const ParseState& st, const std::vector<ModTok>& v)
{
    for (const ModTok& t : v)
        st.error(t.pos, "an interface member cannot have the modifier '" + t.word + "'");
}
void iface_own_mods_error(const ParseState& st, const std::vector<ModTok>& v)
{
    for (const ModTok& t : v)
        st.error(t.pos, "an interface cannot have the modifier '" + t.word + "'");
}

struct iface_member_p
{
    static constexpr auto rule = dsl::position + dsl::opt(dsl::peek(dsl::lit_c<'['>) >> dsl::p<attrs_p>)
        + dsl::opt(dsl::peek(modifier_set) >> dsl::p<mods_p>)
        + (dsl::eof >> dsl::error<e_unexpected_eof> | dsl::peek(dsl::lit_c<'~'>) >> dsl::p<dtor_p>
            | dsl::peek(id) >> dsl::p<typed_member>
            | dsl::peek(dsl::lit_c<'$'>) >> dsl::p<typed_member> | dsl::else_ >> dsl::error<e_member>(junk));
    static constexpr auto value = lexy::bind(
        lexy::callback<Member>([](const ParseState& st, const char*, const auto& ann, const auto& mods, Member&& m)
    {
        if (const Annot* a = annot_of(ann))
            st.error(a->pos, "an interface member cannot have an attribute");
        iface_mods_error(st, mods);
        return std::move(m);
    }),
        lexy::parse_state, lexy::values);
};

struct iface_members_p
{
    static constexpr auto closer = dsl::eof | dsl::lit_c<'}'>;
    static constexpr auto item = dsl::peek_not(closer) >> dsl::try_(dsl::p<iface_member_p>, dsl::p<sync_member>);
    static constexpr auto rule = dsl::opt(dsl::peek_not(closer) >> dsl::list(item)) + dsl::try_(dsl::lit_c<'}'>);
    static constexpr auto value = lexy::as_list<std::vector<Member>>;
};

// ": Base, ..." after the name of an interface. The value is the position of the ":".
struct iface_bases_p
{
    static constexpr auto rule = dsl::position + dsl::lit_c<':'> + dsl::p<bases_p>;
    static constexpr auto value =
        lexy::callback<const char*>([](const char* pos, std::vector<std::string>&&) { return pos; });
};

std::vector<Member> iface_members_of(lexy::nullopt)
{
    return {};
}
std::vector<Member> iface_members_of(std::vector<Member>&& v)
{
    return std::move(v);
}

// "interface Name { Ret Method(params); ... }". The value is a TopItem.
struct interface_p
{
    static constexpr auto rule = dsl::position + kw_interface + dsl::position + dsl::p<name_p<e_iface_name>>
        + dsl::opt(dsl::peek(dsl::lit_c<':'>) >> dsl::p<iface_bases_p>) + dsl::lit_c<'{'> + dsl::p<iface_members_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<TopItem>([](const ParseState& st, const char* pos, const char* name_pos, std::string&& name,
                                    const auto& bases, auto&& members)
    {
        if constexpr (!std::is_same_v<std::decay_t<decltype(bases)>, lexy::nullopt>)
            st.error(bases, "an interface cannot have a base list in this version");
        TopItem t;
        t.kind = TopItem::IsInterface;
        ast::InterfaceDef& d = t.iface;
        check_name(st, name_pos, name);
        d.name = std::move(name);
        d.pos = st.offset(pos);
        d.line = st.map->line(d.pos);
        std::set<std::string> seen;
        for (Member& m : iface_members_of(std::move(members)))
        {
            if (m.kind == Member::IsMethod)
            {
                if (!m.method.op.empty())
                    st.error(st.base + m.method.pos, "an operator needs a class or a struct");
                if (m.no_body_pos == nullptr)
                    st.error(st.base + m.method.body_span.begin,
                        "an interface method cannot have a body; end the declaration with ';'");
                if (!seen.insert(m.method.name).second)
                    st.error(st.base + m.method.pos,
                        "duplicate method '" + m.method.name + "' in interface '" + d.name + "'");
                d.methods.push_back(std::move(m.method));
            }
            else if (m.kind == Member::IsField)
                st.error(st.base + m.field.pos, "an interface cannot have a field; it has methods only");
            else if (m.kind == Member::IsProp)
                st.error(st.base + m.prop.pos, "an interface cannot have a property; it has methods only");
            else if (m.kind == Member::IsCtor)
                st.error(st.base + m.ctor.pos, "an interface cannot have a constructor");
            else if (m.kind == Member::IsDtor)
                st.error(st.base + m.method.pos, "an interface cannot have a destructor");
        }
        return t;
    }),
        lexy::parse_state, lexy::values);
};

// One or more modifiers, then the whole word "interface". It does not read ahead past the
// modifiers, so a name that only contains "interface" does not match.
constexpr auto modifier_then_interface =
    dsl::peek(modifier_set) + dsl::while_(dsl::peek(modifier_set) >> modifier_set + dsl::whitespace(ws_rule))
    + kw_interface;

// Modifiers before "interface" at file level: each one is an error.
struct modded_interface_p
{
    static constexpr auto rule = dsl::p<mods_p> + dsl::must(dsl::peek(kw_interface)).error<e_iface_kw> + dsl::p<interface_p>;
    static constexpr auto value = lexy::bind(
        lexy::callback<TopItem>([](const ParseState& st, const std::vector<ModTok>& mods, TopItem&& t)
    {
        iface_own_mods_error(st, mods);
        return std::move(t);
    }),
        lexy::parse_state, lexy::values);
};

struct class_p
{
    static constexpr auto rule = dsl::position + dsl::opt(dsl::peek(dsl::lit_c<'['>) >> dsl::p<class_attrs_p>)
        + (dsl::peek(kw_interface) >> dsl::p<interface_p>
            | dsl::peek(dsl::literal_set(kw_class, kw_struct, kw_abstract))
                >> (abstract_prefix
                    + (dsl::peek(kw_interface) >> dsl::p<interface_p>
                        | dsl::else_ >> (dsl::must(dsl::peek(class_or_struct)).error<e_abstract_target>
                            + dsl::capture(class_or_struct) + class_rest)))
            | dsl::peek(dsl::lit_c<';'>) >> dsl::p<module_attrs_end_p>
            | dsl::else_ >> (dsl::opt(dsl::peek(modifier_set) >> dsl::p<mods_p>) + dsl::p<typed_member>));
    static constexpr auto value =
        lexy::bind(lexy::callback<TopItem>(
                       [](const ParseState& st, const char*, const auto& attrs, TopItem&& t)
    {
        if (const ClassAttrs* a = class_attrs_of(attrs))
            st.error(a->pos, "an interface cannot have an attribute");
        return std::move(t);
    },
                       [](const ParseState& st, const char*, const auto& attrs, const auto& abs, TopItem&& t)
    {
        if (const ClassAttrs* a = class_attrs_of(attrs))
            st.error(a->pos, "an interface cannot have an attribute");
        if (const char* ap = abstract_pos_of(abs))
            st.error(ap, "an interface cannot have the modifier 'abstract'");
        return std::move(t);
    },
                       [](const ParseState& st, const char* pos, const auto& attrs, const auto& abs, const auto& kw,
                           const char* /*name_pos*/, std::string&& name, lexy::nullopt)
    {
        if (const char* ap = abstract_pos_of(abs))
            st.error(ap, "a class stub cannot be 'abstract'");
        return make_stub(st, pos, class_attrs_of(attrs), std::distance(kw.begin(), kw.end()) == 6,
            std::move(name), false);
    },
                       [](const ParseState& st, const char* pos, const auto& attrs, const auto& abs, const auto& kw,
                           const char*, std::string&& base)
    {
        if (const char* ap = abstract_pos_of(abs))
            st.error(ap, "a class stub cannot be 'abstract'");
        return make_stub(st, pos, class_attrs_of(attrs), std::distance(kw.begin(), kw.end()) == 6,
            std::move(base), true);
    },
                       [](const ParseState& st, const char* pos, const auto& attrs, const auto& abs, const auto& kw,
                           const char* name_pos, std::string&& name, auto&& base, auto&& members)
    {
        const ClassAttrs* a = class_attrs_of(attrs);
        const bool is_struct = std::distance(kw.begin(), kw.end()) == 6;
        const char* abstract_at = abstract_pos_of(abs);
        if (abstract_at && is_struct)
            st.error(abstract_at, "a struct cannot be 'abstract'; only a class can be");
        TopItem t;
        t.kind = TopItem::IsClass;
        ast::ClassDef& c = t.cls;
        if (name.find_first_of("*<:") != std::string::npos)
            st.error(name_pos, "a script class needs a plain name");
        check_name(st, name_pos, name);
        c.name = std::move(name);
        if constexpr (!std::is_same_v<std::decay_t<decltype(base)>, lexy::nullopt>)
        {
            if (!base.empty())
                c.base = std::move(base.front());
            for (size_t i = 1; i < base.size(); ++i)
                c.interfaces.push_back(std::move(base[i]));
        }
        c.pos = st.offset(pos);
        c.line = st.map->line(c.pos);
        // A struct is the only script value type (design 5.3, 7.10.1).
        c.is_value = is_struct;
        c.is_abstract = abstract_at != nullptr && !is_struct;
        if (a)
            c.cpp_prelude = a->cpp_prelude;
        if (a)
            for (const auto& tag : a->tags)
                c.attributes.push_back(tag.second);
        if (a && a->is_attribute)
            st.error(a->pos, "an attribute class has no body: declare it as '[attribute] class " + c.name + ";'");
        else if (a)
            for (const auto& u : a->used)
                if (u.second != "cpp_prelude")
                    st.error(u.first,
                            u.second == "value" ? std::string(kValueRemoved)
                                            : "attribute '" + u.second + "' is not allowed on a script class");
        add_members(st, c, std::move(members));
        return t;
    },
                       [](const ParseState& st, const char*, const auto& attrs, ModuleAttrsMark mark)
    {
        reject_tags(st, class_attrs_of(attrs));
        return make_module_attrs(st, mark.pos, class_attrs_of(attrs));
    },
                       [](const ParseState& st, const char* pos, const auto& attrs, const auto& mods, Member&& member)
    {
        const ClassAttrs* a = class_attrs_of(attrs);
        reject_tags(st, a);
        TopItem t;
        // The attributes are checked here, not in finish_member.
        // A body that is missing has its own error here, not the error of a class method.
        const char* no_body = member.no_body_pos;
        member.no_body_pos = nullptr;
        Member m = finish_member(st, lexy::nullopt{}, mods, std::move(member));
        m.no_body_pos = no_body;
        if (m.no_body_pos && m.kind == Member::IsMethod && m.method.name.rfind("convert_", 0) == 0)
            st.error(m.no_body_pos,
                "method '" + m.method.name + "' needs a body; write '{ ... }'");
        // "T convert_<var>(Old old) { }" at file level is the convert hook of a module
        // variable. Other functions must be members of a class.
        if (m.kind == Member::IsMethod && m.method.op.empty())
        {
            // A function at file level has no modifier.
            // The function "main" has one error for each wrong form (see kMainSignatureError).
            const bool is_main = m.method.name == "main";
            // "private" hides the function from importers. The other access words are errors.
            if (m.access_pos && (is_main || m.method.access != ast::Access::Private))
                st.error(m.access_pos, is_main ? ast::kMainSignatureError : "a free function has no access word");
            if (m.async_pos)
                st.error(m.async_pos, is_main ? ast::kMainSignatureError : "a free function cannot be 'async'");
            if (m.static_pos)
                st.error(m.static_pos, is_main ? ast::kMainSignatureError : "a free function cannot be 'static'");
            for (const auto& w : m.kind_words)
                st.error(w.first, "a free function cannot be '" + w.second + "'");
            m.method.is_override = m.method.is_virtual = m.method.is_new = m.method.is_async = false;
            m.method.is_abstract = false;
        }
        if (m.kind == Member::IsMethod && m.method.name.rfind("convert_", 0) == 0)
        {
            if (a && !a->used.empty())
                st.error(a->used.front().first,
                    "attribute '" + a->used.front().second + "' is not allowed on a convert hook");
            t.kind = TopItem::IsHook;
            t.hook = std::move(m.method);
            return t;
        }
        if (m.kind == Member::IsMethod && m.method.op.empty())
        {
            // A free function at file level.
            if (m.no_body_pos)
                st.error(m.no_body_pos, "function '" + m.method.name + "' needs a body; write '{ ... }'");
            t.kind = TopItem::IsFn;
            t.fn = std::move(m.method);
            return t;
        }
        if (m.kind != Member::IsField)
        {
            st.error(pos,
                m.kind == Member::IsMethod && !m.method.op.empty()
                    ? "an operator needs a class or a struct"
                    : "expected 'class', 'struct', 'enum' or a variable 'Type name = init;'");
            return t;
        }
        t.kind = m.const_pos ? TopItem::IsConst : TopItem::IsVar;
        t.var = std::move(m.field);
        if (m.static_pos)
            st.error(m.static_pos, m.const_pos ? "a module constant is static already; remove 'static'"
                                               : "a module variable is static already; remove 'static'");
        if (m.const_pos && !t.var.has_init)
            st.error(m.const_pos, "the constant '" + t.var.name + "' needs a value");
        if (a && m.const_pos)
        {
            for (const auto& u : a->used)
                st.error(u.first, "attribute '" + u.second + "' is not allowed on a constant");
            for (const auto& tag : a->tags)
                st.error(tag.first, "attribute '" + tag.second + "' is not allowed on a constant");
        }
        else if (a)
        {
            t.var.alias = a->alias;
            for (const auto& u : a->used)
                if (u.second == "value")
                    st.error(u.first, kValueRemoved);
                else if (u.second != "alias")
                    st.error(u.first, "attribute '" + u.second + "' is not allowed on a module variable");
        }
        return t;
    }),
            lexy::parse_state, lexy::values);
};

// ---------------------------------------------------------------- enums
// The end of an enumerator value: a "," or the closing brace at depth zero.
struct enum_value_cfg
{
    static constexpr auto open = dsl::lit_c<'='>;
    static constexpr auto term = dsl::lit_c<','> / dsl::lit_c<'}'>;
};

struct enumerator_p
{
    static constexpr auto rule = dsl::position + dsl::p<name_p<e_enumerator>>
        + dsl::opt(dsl::peek(dsl::lit_c<'='>) >> dsl::p<scan_until<enum_value_cfg>>);
    static constexpr auto value =
        lexy::bind(lexy::callback<ast::Enumerator>(
                       [](const ParseState& st, const char* pos, std::string&& name, lexy::nullopt)
    {
        ast::Enumerator e;
        e.name = std::move(name);
        e.pos = st.offset(pos);
        e.line = st.map->line(e.pos);
        return e;
    },
                       [](const ParseState& st, const char* pos, std::string&& name, Span value)
    {
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

struct enumerators_p
{
    static constexpr auto rule = dsl::lit_c<'{'>
        + dsl::terminator(dsl::lit_c<'}'>).opt_list(dsl::p<enumerator_p>, dsl::trailing_sep(dsl::lit_c<','>));
    static constexpr auto value = lexy::as_list<std::vector<ast::Enumerator>>;
};

// "enum Name { A, B = 5 }" or "enum Name : type { ... }".
struct enum_p
{
    static constexpr auto rule = dsl::position + kw_enum + dsl::position + dsl::p<name_p<e_enum_name>>
        + dsl::opt(dsl::lit_c<':'> >> (dsl::position + dsl::p<scan_type<ret_term>>)) + dsl::p<enumerators_p>;
    static constexpr auto value =
        lexy::bind(lexy::callback<TopItem>(
                       [](const ParseState& st, const char* pos, const char*, std::string&& name, lexy::nullopt,
                           std::vector<ast::Enumerator>&& items)
    {
        TopItem t;
        t.kind = TopItem::IsEnum;
        t.en.name = std::move(name);
        t.en.pos = st.offset(pos);
        t.en.line = st.map->line(t.en.pos);
        t.en.items = std::move(items);
        return t;
    },
                       [](const ParseState& st, const char* pos, const char*, std::string&& name, const char* type_pos,
                           Span type, std::vector<ast::Enumerator>&& items)
    {
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

// "private" before "class", "struct", "abstract" or "enum" at file level. Importers do not see
// the name. A variable, a constant and a free function read "private" as their access word.
constexpr auto private_then_type = kw_private + dsl::whitespace(ws_rule)
    + dsl::literal_set(kw_class, kw_struct, kw_abstract, kw_enum);

struct private_type_p
{
    static constexpr auto rule = kw_private + (dsl::peek(kw_enum) >> dsl::p<enum_p> | dsl::else_ >> dsl::p<class_p>);
    static constexpr auto value = lexy::callback<TopItem>([](TopItem&& t)
    {
        t.is_private = true;
        return std::move(t);
    });
};

// ---------------------------------------------------------------- file
struct decl
{
    static constexpr auto rule = dsl::peek(kw_using) >> dsl::p<import_p> | dsl::peek(kw_enum) >> dsl::p<enum_p>
        | dsl::peek(dsl::literal_set(dsl::lit_c<'['>, dsl::lit_c<';'>, kw_class, kw_struct, kw_interface))
            >> dsl::p<class_p>
        | dsl::peek(private_then_type) >> dsl::p<private_type_p>
        | dsl::peek(modifier_then_interface) >> dsl::p<modded_interface_p>
        | dsl::peek(kw_static_assert) >> dsl::p<static_assert_top_p>
        | dsl::peek(id) >> dsl::p<class_p> | dsl::peek(dsl::lit_c<'$'>) >> dsl::p<class_p>
        | dsl::else_ >> dsl::error<e_top>(junk);
    static constexpr auto value = lexy::callback<TopItem>([](TopItem&& t) { return std::move(t); });
};

// Skips a broken declaration. It stops before the next "using" or "class".
struct sync_decl : lexy::token_production
{
    static constexpr auto rule =
        dsl::loop(dsl::eof >> dsl::break_ | // the end of the file is not a new error after a broken decl
            dsl::peek(dsl::literal_set(kw_using, kw_class, kw_struct, kw_enum, kw_interface)) >> dsl::break_
            | dsl::else_ >> dsl::p<skip_item>);
    static constexpr auto value = lexy::noop;
};

struct file_p
{
    static constexpr auto whitespace = ws_rule;
    static constexpr auto rule = dsl::terminator(dsl::eof).opt_list(dsl::try_(dsl::p<decl>, dsl::p<sync_decl>));
    static constexpr auto value = lexy::as_list<std::vector<TopItem>>;
};

} // namespace

const std::vector<std::string>& class_attribute_names()
{
    static const std::vector<std::string> names = []
    {
        std::vector<std::string> r;
        for (const ClassAttr& a : kClassAttrs)
            r.push_back(a.name);
        return r;
    }();
    return names;
}

ast::Script parse_script_array(std::string_view text, const LineMap& map, Diagnostics& diag,
    std::string_view host_array_tpl)
{
    const size_t n_items = diag.items.size(), n_warnings = diag.warnings.size(), n_holes = diag.member_holes.size();
    ast::Script sc = parse_script(text, map, diag, host_array_tpl);
    std::string tpl(host_array_tpl);
    for (const ast::ModuleAttrs& m : sc.module_attrs)
        if (m.has_array)
            tpl = m.array_tpl; // a repeat is an error that the caller reports; the last one counts here
    if (tpl == host_array_tpl)
        return sc;
    diag.items.resize(n_items);
    diag.warnings.resize(n_warnings);
    diag.member_holes.resize(n_holes);
    return parse_script(text, map, diag, tpl);
}

namespace
{

// True when the type text names "name" (a name after "::" is a member name and does not count).
bool mentions_name(const std::string& type, const std::string& name)
{
    return apply_aliases(AliasMap{{name, "\x02"}}, type) != type;
}

// True when the name is a C++ keyword. A keyword is not a valid alias name in the output.
bool is_cpp_keyword(const std::string& n)
{
    static const std::set<std::string> kw = {"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
        "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
        "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield",
        "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
        "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while", "xor", "xor_eq"};
    return kw.count(n) != 0;
}

// True when the name is a builtin type word of the language (int, uint, string, byte and so on).
bool is_builtin_type_word(const std::string& n)
{
    std::string error;
    return n == "Task" || canonical_type(n, &error) != n || !error.empty();
}

// True when the type text of "a" reaches the name "target" through the aliases of "list" that
// are not in "seen". Used to tell a cycle from a plain use of a later alias.
bool alias_reaches(const std::vector<ast::TypeAlias>& list, const std::string& type, const std::string& target,
    std::set<std::string>& seen)
{
    if (mentions_name(type, target))
        return true;
    for (const ast::TypeAlias& b : list)
        if (mentions_name(type, b.name) && seen.insert(b.name).second && alias_reaches(list, b.type, target, seen))
            return true;
    return false;
}

// Checks the aliases of one scope and adds them to "scope". "scope" starts with the aliases of the
// outer scope: an alias of the inner scope replaces the alias of the outer scope with the same name.
// An alias that has an error stays out of the map. An alias can use only an earlier alias of the
// same scope: a later alias, and so a cycle, is an error.
void resolve_aliases(const ParseState& st, std::vector<ast::TypeAlias>& list, const std::set<std::string>& class_names,
    AliasMap& scope)
{
    std::set<std::string> seen;
    std::set<std::string> rejected; // names of aliases of this scope that have an error
    for (size_t i = 0; i < list.size(); ++i)
    {
        ast::TypeAlias& a = list[i];
        const char* at = st.base + a.pos;
        std::string later; // the first later alias that the type uses
        for (size_t k = i + 1; k < list.size() && later.empty(); ++k)
            if (list[k].name != a.name && mentions_name(a.type, list[k].name))
                later = list[k].name;
        std::string bad; // an earlier alias that has an error
        for (const std::string& r : rejected)
            if (r != a.name && mentions_name(a.type, r))
                bad = r;
        if (!seen.insert(a.name).second)
        {
            st.error(at, "duplicate type alias '" + a.name + "'");
            continue;
        }
        std::string error;
        if (class_names.count(a.name))
            error = "the type alias '" + a.name + "' has the name of a class";
        else if (is_cpp_keyword(a.name) || is_builtin_type_word(a.name))
            error = "'" + a.name + "' is a type of the language or a C++ keyword. It is not a name for a type alias";
        else if (mentions_name(a.type, a.name))
            error = "the type alias '" + a.name + "' uses itself";
        else if (!later.empty())
        {
            std::set<std::string> visited;
            if (alias_reaches(list, a.type, a.name, visited))
                error = "the type alias '" + a.name + "' is cyclic";
            else
                error = "the type alias '" + a.name + "' uses a later alias '" + later + "'. Write the alias first";
        }
        else if (!bad.empty())
            error = "the type alias '" + a.name + "' uses the alias '" + bad + "' that has an error";
        if (!error.empty())
        {
            st.error(at, error);
            rejected.insert(a.name);
            continue;
        }
        a.resolved = apply_aliases(scope, a.type);
        scope[a.name] = a.resolved;
    }
}

// Replaces the alias names in the types of a signature.
struct AliasRewriter
{
    const AliasMap& map;
    void type(std::string& t) const
    {
        if (!t.empty())
            t = apply_aliases(map, t);
    }
    void params(std::vector<ast::Param>& ps) const
    {
        for (ast::Param& p : ps)
            type(p.type);
    }
    void method(ast::Method& m) const
    {
        type(m.ret);
        params(m.params);
    }
    void cls(ast::ClassDef& c) const
    {
        type(c.base);
        for (std::string& i : c.interfaces)
            type(i);
        for (ast::Field& f : c.fields)
            type(f.type);
        for (ast::Method& m : c.methods)
            method(m);
        for (ast::Method& m : c.statics)
            method(m);
        for (ast::Method& m : c.dtors)
            method(m);
        for (ast::Ctor& k : c.ctors)
            params(k.params);
        for (ast::Property& p : c.props)
            type(p.type);
    }
};

} // namespace

// A static property (gap G23) becomes the static methods "get_Name" and "set_Name" of the class.
// "get; set;" adds a hidden static field "Class____p_Name" (a module variable). The translator
// makes a read a call of the getter and a write statement a call of the setter (ClassDef::sprops).
void static_props(ast::ClassDef& c, ast::Script& script)
{
    using namespace ast;
    for (auto it = c.props.begin(); it != c.props.end();)
    {
        if (!it->is_static)
        {
            ++it;
            continue;
        }
        Property p = std::move(*it);
        it = c.props.erase(it);
        const std::string hidden = "__p_" + p.name;
        if (p.is_auto())
        {
            Field f;
            f.name = c.name + "__" + hidden;
            f.owner = c.name;
            f.type = p.type;
            f.init = std::move(p.init);
            f.init_span = p.init_span;
            f.has_init = p.has_init;
            f.access = Access::Private;
            f.line = p.line;
            f.pos = p.pos;
            script.vars.push_back(std::move(f));
        }
        auto accessor = [&](bool is_get)
        {
            Method m;
            m.is_static = true;
            m.access = is_get ? p.get_access : p.set_access;
            m.name = (is_get ? "get_" : "set_") + p.name;
            m.ret = is_get ? p.type : std::string(); // an empty "ret" is "void"
            if (!is_get)
                m.params.push_back({"value", p.type});
            m.pos = is_get ? p.get_pos : p.set_pos;
            m.line = m.end_line = p.line;
            m.end_pos = m.pos;
            std::optional<BlockStmt>& body = is_get ? p.get_body : p.set_body;
            if (body)
                m.body = std::move(*body);
            else if (is_get)
                m.body.stmts.push_back(make_stmt(ReturnStmt{make_expr(NameExpr{{hidden}, "", {}, false, m.pos}), m.pos}));
            else
            {
                BinaryExpr b;
                b.op = "=";
                b.lhs = make_expr(NameExpr{{hidden}, "", {}, false, m.pos});
                b.rhs = make_expr(NameExpr{{"value"}, "", {}, false, m.pos});
                b.pos = m.pos;
                m.body.stmts.push_back(make_stmt(ExprStmt{make_expr(std::move(b)), m.pos}));
            }
            c.statics.push_back(std::move(m));
        };
        if (p.has_get)
            accessor(true);
        if (p.has_set)
            accessor(false);
        p.get_body.reset();
        p.set_body.reset();
        p.init.reset();
        c.sprops.push_back(std::move(p));
    }
}

ast::Script parse_script(std::string_view text, const LineMap& map, Diagnostics& diag, std::string_view array_tpl)
{
    ast::Script script;
    const size_t first_item = diag.items.size();
    const ParseState st{text.data(), &map, &diag, text.size(), array_tpl};
    const auto input = lexy::string_input<Encoding>(text.data(), text.size());
    auto result = parse_with_diag<file_p>(input, st);
    if (result.has_value())
    {
        for (TopItem& t : std::move(result).value())
        {
            if (t.kind == TopItem::IsImport)
                script.imports.push_back(std::move(t.import));
            else if (t.kind == TopItem::IsUsingNs)
                script.using_ns.push_back(std::move(t.using_ns));
            else if (t.kind == TopItem::IsAlias)
                script.aliases.push_back(std::move(t.alias));
            else if (t.kind == TopItem::IsClass)
            {
                t.cls.is_private = t.is_private;
                script.classes.push_back(std::move(t.cls));
            }
            else if (t.kind == TopItem::IsInterface)
            {
                bool dup = false;
                for (const ast::InterfaceDef& o : script.interfaces)
                    dup = dup || o.name == t.iface.name;
                if (dup)
                    st.error(st.base + t.iface.pos, "duplicate interface '" + t.iface.name + "'");
                else
                    script.interfaces.push_back(std::move(t.iface));
            }
            else if (t.kind == TopItem::IsStub && t.stub.is_attribute)
                script.attribute_classes.push_back(std::move(t.stub));
            else if (t.kind == TopItem::IsStub)
                script.stubs.push_back(std::move(t.stub));
            else if (t.kind == TopItem::IsEnum)
            {
                t.en.is_private = t.is_private;
                script.enums.push_back(std::move(t.en));
            }
            else if (t.kind == TopItem::IsVar)
                script.vars.push_back(std::move(t.var));
            else if (t.kind == TopItem::IsConst)
                script.consts.push_back(std::move(t.var));
            else if (t.kind == TopItem::IsStaticAssert)
                script.static_asserts.push_back(std::move(t.sa));
            else if (t.kind == TopItem::IsHook)
                script.var_hooks.push_back(std::move(t.hook));
            else if (t.kind == TopItem::IsFn)
                script.fns.push_back(std::move(t.fn));
            else if (t.kind == TopItem::IsModuleAttrs)
                script.module_attrs.push_back(std::move(t.module_attrs));
        }
    }
    // The type aliases. They replace the alias name in each type of the scope: the types of the
    // structure now, the types in the bodies when the parser reads the bodies (ParseInput::aliases).
    AliasMap module_aliases;
    std::set<std::string> class_names;
    for (const ast::ClassDef& c : script.classes)
        class_names.insert(c.name);
    for (const ast::EnumDef& e : script.enums)
        class_names.insert(e.name);
    for (const ast::InterfaceDef& d : script.interfaces)
        class_names.insert(d.name);
    for (const ast::Stub& s : script.stubs)
        class_names.insert(s.name);
    for (const ast::Stub& s : script.attribute_classes)
        class_names.insert(s.name);
    resolve_aliases(st, script.aliases, class_names, module_aliases);
    std::vector<AliasMap> class_aliases;
    for (ast::ClassDef& c : script.classes)
    {
        class_aliases.push_back(module_aliases);
        resolve_aliases(st, c.aliases, class_names, class_aliases.back());
        AliasRewriter{class_aliases.back()}.cls(c);
        // A class constant is a module constant with the name "Class__Name" and the owner "Class".
        // The translator gives "Class.Name" and a bare "Name" in the class this name.
        for (ast::Field& f : c.consts)
        {
            AliasRewriter{class_aliases.back()}.type(f.type);
            f.owner = c.name;
            f.name = c.name + "__" + f.name;
            script.consts.push_back(std::move(f));
        }
        c.consts.clear();
        for (ast::StaticAssert& a : c.static_asserts)
        {
            a.owner = c.name;
            script.static_asserts.push_back(std::move(a));
        }
        c.static_asserts.clear();
        // A static field (gap G23) is a module variable of the owner module with the name
        // "Class__Name" and the owner "Class". The module block holds it, so a reload keeps it.
        for (ast::Field& f : c.svars)
        {
            AliasRewriter{class_aliases.back()}.type(f.type);
            f.owner = c.name;
            f.name = c.name + "__" + f.name;
            script.vars.push_back(std::move(f));
        }
        c.svars.clear();
    }
    if (!module_aliases.empty())
    {
        const AliasRewriter rw{module_aliases};
        for (ast::EnumDef& e : script.enums)
            rw.type(e.underlying);
        for (ast::Field& f : script.vars)
            rw.type(f.type);
        for (ast::Field& f : script.consts)
            rw.type(f.type);
        for (ast::Method& m : script.var_hooks)
            rw.method(m);
        for (ast::Method& m : script.fns)
            rw.method(m);
        for (ast::InterfaceDef& d : script.interfaces)
            for (ast::Method& m : d.methods)
                rw.method(m);
    }
    // The span-then-call step: parse each body and each initializer.
    ParseInput in{text, &map, array_tpl, &module_aliases};
    for (ast::EnumDef& e : script.enums)
        for (ast::Enumerator& m : e.items)
            if (m.has_value && !only_blank(span_text(st, m.value_span)))
                m.value = parse_expression(in, m.value_span, diag);
    for (std::vector<ast::Field>* list : {&script.vars, &script.consts})
        for (ast::Field& f : *list)
            if (f.has_init && !only_blank(span_text(st, f.init_span)))
                f.init = parse_expression(in, f.init_span, diag);
    for (ast::StaticAssert& a : script.static_asserts)
        a.args = parse_expression_list(in, a.args_span, diag);
    // "=> e;" is "{ return e; }", or "{ e; }" for a void function.
    auto arrow_block = [&](Span span, bool is_void)
    {
        ast::BlockStmt b;
        b.pos = span.begin;
        b.end_pos = span.end;
        if (only_blank(span_text(st, span)))
            return b;
        ast::ExprP e = parse_expression(in, span, diag);
        if (!e)
            return b;
        if (is_void)
            b.stmts.push_back(ast::make_stmt(ast::ExprStmt{std::move(e), span.begin}));
        else
            b.stmts.push_back(ast::make_stmt(ast::ReturnStmt{std::move(e), span.begin}));
        return b;
    };
    auto parse_defaults = [&](std::vector<ast::Param>& params)
    {
        for (ast::Param& p : params)
            if (p.def && !only_blank(span_text(st, p.def->span)))
                p.def->expr = parse_expression(in, p.def->span, diag);
    };
    auto method_body = [&](ast::Method& m)
    {
        parse_defaults(m.params);
        m.body = m.expr_body ? arrow_block(m.body_span, m.ret.empty()) : parse_block(in, m.body_span, diag);
    };
    for (ast::Method& m : script.var_hooks)
        method_body(m);
    for (ast::Method& m : script.fns)
        method_body(m);
    for (size_t ci = 0; ci < script.classes.size(); ++ci)
    {
        ast::ClassDef& c = script.classes[ci];
        in.aliases = &class_aliases[ci];
        for (ast::Method& m : c.methods)
            method_body(m);
        for (ast::Method& m : c.statics)
            method_body(m);
        for (ast::Method& d : c.dtors)
            method_body(d);
        for (ast::Ctor& k : c.ctors)
        {
            parse_defaults(k.params);
            k.body = parse_block(in, k.body_span, diag);
            if (k.has_base_call)
                k.base_args = parse_expression_list(in, k.base_span, diag);
        }
        for (ast::Field& f : c.fields)
            if (f.has_init && !only_blank(span_text(st, f.init_span)))
                f.init = parse_expression(in, f.init_span, diag);
        for (ast::Property& p : c.props)
        {
            if (p.get_body)
                p.get_body = p.get_is_expr ? arrow_block(p.get_span, false) : parse_block(in, p.get_span, diag);
            if (p.set_body)
                p.set_body = parse_block(in, p.set_span, diag);
            if (p.has_init && !only_blank(span_text(st, p.init_span)))
                p.init = parse_expression(in, p.init_span, diag);
        }
        static_props(c, script);
    }
    // An unterminated comment or literal reads to the end of the file. The "unexpected end of
    // file" errors from this place on are not new faults. Drop them.
    uint32_t cut = UINT32_MAX;
    for (size_t i = first_item; i < diag.items.size(); ++i)
        if (diag.items[i].msg.compare(0, 12, "unterminated") == 0)
            cut = std::min(cut, diag.items[i].pos);
    // A class that is not closed gives "unexpected end of file" and "expected '}'" in one place.
    // Keep the first.
    auto is_eof_error = [&](const Diag& d) { return d.msg == "unexpected end of file"; };
    const std::vector<Diag> before_cut(diag.items.begin() + static_cast<std::ptrdiff_t>(first_item), diag.items.end());
    diag.items.erase(std::remove_if(diag.items.begin() + static_cast<std::ptrdiff_t>(first_item), diag.items.end(),
                         [&](const Diag& d)
    {
        if ((is_eof_error(d) || d.msg == "expected '}'") && d.pos >= cut)
            return true;
        if (d.msg != "expected '}'")
            return false;
        for (const Diag& o : before_cut)
            if (is_eof_error(o) && o.pos == d.pos)
                return true;
        return false;
    }),
        diag.items.end());
    // The errors of the structure come before the errors of the bodies. Sort them.
    std::stable_sort(diag.items.begin() + static_cast<std::ptrdiff_t>(first_item), diag.items.end(),
        [](const Diag& a, const Diag& b) { return a.pos < b.pos; });
    return script;
}

} // namespace hotc
