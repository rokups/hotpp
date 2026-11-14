// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The table of the resolved names (refs.h). One walk over each body keeps a stack of scopes, as
// the lint pass does (lint.cpp). The walk also gives the script type of each expression that it
// can know, so "obj.M()" finds the method M of the class of "obj".

#include "refs.h"

#include <algorithm>
#include <map>

namespace hotc
{

using namespace ast;

namespace
{

template <class... Fs> struct Overloaded : Fs...
{
    using Fs::operator()...;
};
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;

bool ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool ident_char(char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

// The bare class name of a script type text: "const $Player?" -> "Player". Empty when the type has
// template arguments or is not a plain name.
std::string plain_type(std::string t)
{
    auto trim = [](std::string& s)
    {
        const size_t b = s.find_first_not_of(" \t");
        const size_t e = s.find_last_not_of(" \t");
        s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    trim(t);
    for (const char* word : {"const ", "ref ", "in ", "out ", "box "})
        if (t.rfind(word, 0) == 0)
        {
            t = t.substr(std::string(word).size());
            trim(t);
        }
    if (!t.empty() && t[0] == '$')
        t = t.substr(1);
    while (!t.empty() && (t.back() == '?' || t.back() == '&' || t.back() == '*' || t.back() == ' '))
        t.pop_back();
    for (char c : t)
        if (!ident_char(c))
            return std::string();
    return t;
}

// The result of the walk of an expression.
struct Value
{
    std::string type; // the script type of the value; empty: not known
    std::string ret; // the script return type when the expression names a function
    const ClassDef* cls = nullptr; // the expression names this class (a scope, not a value)
    const EnumDef* en = nullptr; // the expression names this enum
};

struct Local
{
    uint32_t pos = 0; // the offset of the declaration name; 0: no ref
    RefKind kind = RefKind::Local;
    std::string type;
};

// A member that a name finds in a class or in a script parent.
struct Member
{
    RefKind kind = RefKind::Field;
    const ClassDef* owner = nullptr;
    std::string type; // the value type, or the return type of a method
    bool callable = false;
    const InterfaceDef* iface = nullptr; // set for a method of a script interface ("owner" is null)
};

class Resolver
{
public:
    explicit Resolver(const RefInputs& in) : in_(in), sc_(*in.script)
    {
        // The last class of a name wins, as in the lowering (base_cx.classes). Two imports can
        // have a class with the same name. The main script and an import cannot.
        for (const ClassDef& c : sc_.classes)
            classes_[c.name] = &c;
        for (const InterfaceDef& d : sc_.interfaces)
            ifaces_[d.name] = &d;
        for (const EnumDef& e : sc_.enums)
            enums_.emplace(e.name, &e);
        for (const Field& f : sc_.consts)
            if (f.owner.empty())
                module_.emplace(f.name, std::make_pair(&f, RefKind::Const));
        for (const Field& f : sc_.vars)
            if (f.owner.empty())
                module_.emplace(f.name, std::make_pair(&f, RefKind::ModuleVar));
        for (const Method& m : sc_.fns)
            fns_.emplace(m.written_name(), &m);
        for (const Field* f : in_.foreign_vars)
            if (f && f->owner.empty())
                module_.emplace(f->name, std::make_pair(f, RefKind::ModuleVar));
        for (const auto* list : {in_.foreign_fns, in_.deep_fns})
            if (list)
                for (const Method& m : *list)
                    fns_.emplace(m.written_name(), &m);
    }

    RefTable run()
    {
        const uint32_t size = static_cast<uint32_t>(in_.text.size());
        for (const ClassDef& c : sc_.classes)
            if (c.pos < size && c.foreign_unit.empty())
                walk_class(c);
        for (const InterfaceDef& d : sc_.interfaces)
            if (d.pos < size)
                walk_iface(d);
        for (const EnumDef& e : sc_.enums)
            if (e.pos < size)
            {
                decl_name(e.pos, e.name, RefKind::Enum, e.name, 64);
                for (const Enumerator& it : e.items)
                    decl_name(it.pos, it.name, RefKind::EnumValue, e.name + "." + it.name, 64);
            }
        for (const Field& f : sc_.consts)
            if (f.pos < size)
                walk_static_field(f, RefKind::Const);
        for (const Field& f : sc_.vars)
            if (f.pos < size)
                walk_static_field(f, f.owner.empty() ? RefKind::ModuleVar : RefKind::StaticField);
        for (const Method& m : sc_.fns)
            if (m.pos < size && m.foreign_unit.empty())
                walk_fn(m, nullptr, RefKind::Function, m.written_name());
        std::stable_sort(refs_.begin(), refs_.end(),
            [](const ResolvedRef& a, const ResolvedRef& b) { return a.offset < b.offset; });
        refs_.erase(std::unique(refs_.begin(), refs_.end(),
                        [](const ResolvedRef& a, const ResolvedRef& b) { return a.offset == b.offset; }),
            refs_.end());
        std::sort(unresolved_.begin(), unresolved_.end(),
            [](const UnresolvedName& a, const UnresolvedName& b) { return a.offset < b.offset; });
        unresolved_.erase(std::unique(unresolved_.begin(), unresolved_.end(),
                              [](const UnresolvedName& a, const UnresolvedName& b) { return a.offset == b.offset; }),
            unresolved_.end());
        std::sort(raw_.begin(), raw_.end());
        return RefTable{std::move(refs_), std::move(unresolved_), std::move(raw_)};
    }

private:
    const RefInputs& in_;
    const Script& sc_;
    std::map<std::string, const ClassDef*> classes_;
    std::map<std::string, const InterfaceDef*> ifaces_;
    std::map<std::string, const EnumDef*> enums_;
    std::map<std::string, std::pair<const Field*, RefKind>> module_;
    std::map<std::string, const Method*> fns_;
    std::vector<ResolvedRef> refs_;
    std::vector<UnresolvedName> unresolved_;
    std::vector<std::pair<uint32_t, uint32_t>> raw_;
    std::vector<std::map<std::string, Local>> scopes_;
    const ClassDef* cls_ = nullptr; // the class of the body; null in a free function

    // ------------------------------------------------------------ the table
    std::string file_of(uint32_t pos) const { return in_.file_of ? in_.file_of(pos) : std::string(); }

    // True when the main script has the word "name" at "pos".
    bool word_at(uint32_t pos, const std::string& name) const
    {
        const std::string_view t = in_.text;
        if (name.empty() || pos >= t.size() || t.compare(pos, name.size(), name) != 0)
            return false;
        const size_t end = pos + name.size();
        return (pos == 0 || !ident_char(t[pos - 1])) && (end >= t.size() || !ident_char(t[end]));
    }

    // The offset of the word "name" at "pos" or in the "limit" bytes after it. 0: not found.
    uint32_t find_word(uint32_t pos, const std::string& name, uint32_t limit) const
    {
        const std::string_view t = in_.text;
        for (uint32_t p = pos; p < t.size() && p <= pos + limit; ++p)
            if (word_at(p, name))
                return p;
        return 0;
    }

    void add(uint32_t pos, const std::string& name, RefKind kind, std::string decl, std::string file, bool is_decl)
    {
        if (pos == 0 || !word_at(pos, name))
            return;
        ResolvedRef r;
        r.offset = pos;
        r.length = static_cast<uint32_t>(name.size());
        r.pos = {in_.map->line(pos), in_.map->col(pos)};
        r.kind = kind;
        r.is_decl = is_decl;
        r.decl = std::move(decl);
        r.decl_file = file.empty() ? in_.main_file : std::move(file);
        r.id = r.decl_file + "#" + r.decl;
        refs_.push_back(std::move(r));
    }

    // A member name after "." with a receiver of no known script type.
    void add_unresolved(uint32_t pos, const std::string& name)
    {
        if (pos == 0 || !word_at(pos, name))
            return;
        UnresolvedName u;
        u.offset = pos;
        u.length = static_cast<uint32_t>(name.size());
        u.pos = {in_.map->line(pos), in_.map->col(pos)};
        unresolved_.push_back(u);
    }

    // A raw body of a ".cpp" script: the pass does not read it.
    void add_raw(const Span& s)
    {
        if (s.end > s.begin && s.end <= in_.text.size())
            raw_.emplace_back(s.begin, s.end);
    }

    // A declaration of the main script: the name is at "pos" or a little after it.
    void decl_name(uint32_t pos, const std::string& name, RefKind kind, std::string decl, uint32_t limit)
    {
        if (const uint32_t p = find_word(pos, name, limit))
            add(p, name, kind, std::move(decl), std::string(), true);
    }

    void add_member(uint32_t pos, const std::string& name, const Member& m)
    {
        if (m.iface)
            add(pos, name, m.kind, m.iface->name + "." + name, file_of(m.iface->pos), false);
        else if (m.kind == RefKind::Method)
        {
            auto [decl, file] = method_root(*m.owner, name);
            add(pos, name, m.kind, std::move(decl), std::move(file), false);
        }
        else
            add(pos, name, m.kind, m.owner->name + "." + name, file_of(m.owner->pos), false);
    }

    // A type name: a class, an interface or an enum. True when the name is one.
    bool add_type(uint32_t pos, const std::string& name)
    {
        if (auto it = classes_.find(name); it != classes_.end())
            add(pos, name, RefKind::Class, name, file_of(it->second->pos), false);
        else if (auto in = ifaces_.find(name); in != ifaces_.end())
            add(pos, name, RefKind::Interface, name, file_of(in->second->pos), false);
        else if (auto en = enums_.find(name); en != enums_.end())
            add(pos, name, RefKind::Enum, name, file_of(en->second->pos), false);
        else
            return false;
        return true;
    }

    // The type names in the text [begin, end) of the main script. A name after a "." is a part of a
    // qualified name, not a script type.
    void types_in(uint32_t begin, uint32_t end)
    {
        const std::string_view t = in_.text;
        end = std::min<uint32_t>(end, static_cast<uint32_t>(t.size()));
        for (uint32_t p = begin; p < end;)
        {
            if (!ident_start(t[p]) || (p > 0 && ident_char(t[p - 1])))
            {
                ++p;
                continue;
            }
            uint32_t q = p;
            while (q < end && ident_char(t[q]))
                ++q;
            uint32_t b = p;
            while (b > begin && (t[b - 1] == ' ' || t[b - 1] == '\t'))
                --b;
            if (b == begin || t[b - 1] != '.')
                add_type(p, std::string(t.substr(p, q - p)));
            p = q;
        }
    }

    // The type text before a declaration name on the same line: "public static Player? p" gives
    // the range of "public static Player? ". The words that are not types give no ref.
    void type_before(uint32_t name_pos)
    {
        const std::string_view t = in_.text;
        if (name_pos == 0 || name_pos > t.size())
            return;
        uint32_t b = name_pos;
        while (b > 0)
        {
            const char c = t[b - 1];
            if (!(ident_char(c) || c == ' ' || c == '\t' || c == '.' || c == '<' || c == '>' || c == ','
                    || c == '[' || c == ']' || c == '?' || c == '*' || c == '&' || c == '$'))
                break;
            --b;
        }
        types_in(b, name_pos);
    }

    // The type text after a word ("new", "is", "as") at "pos": up to the first char that is not
    // a part of a type.
    void type_after(uint32_t pos)
    {
        const std::string_view t = in_.text;
        uint32_t e = pos;
        while (e < t.size() && ident_char(t[e]))
            ++e;
        int depth = 0;
        while (e < t.size())
        {
            const char c = t[e];
            if (c == '<')
                ++depth;
            else if (c == '>' && depth > 0)
                --depth;
            else if (!(ident_char(c) || c == ' ' || c == '\t' || c == '.' || c == '?' || c == '[' || c == ']' || c == '$'
                         || (depth > 0 && c == ',')))
                break;
            ++e;
        }
        types_in(pos, e);
    }

    // ------------------------------------------------------------ the symbols
    const ClassDef* class_named(const std::string& type) const
    {
        const std::string n = plain_type(type);
        auto it = classes_.find(n);
        return it == classes_.end() ? nullptr : it->second;
    }

    const ClassDef* parent_of(const ClassDef& c) const
    {
        return c.base.empty() || c.base == c.name ? nullptr : class_named(c.base);
    }

    static std::string type_of(const Field& f) { return f.script_type.empty() ? f.type : f.script_type; }
    static std::string type_of(const Param& p) { return p.script_type.empty() ? p.type : p.script_type; }
    static std::string ret_of(const Method& m) { return m.script_ret.empty() ? m.ret : m.script_ret; }

    // A member of the class "c" or of a script parent: an instance member, a static member or a
    // class constant. "statics": also the static members (a "C.x" scope or a bare name).
    std::optional<Member> member(const ClassDef& c, const std::string& n) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
        {
            for (const Field& f : d->fields)
                if (f.name == n)
                    return Member{RefKind::Field, d, type_of(f), false};
            for (const Property& p : d->props)
                if (p.name == n)
                    return Member{RefKind::Property, d, p.script_type.empty() ? p.type : p.script_type, false};
            for (const Method& m : d->methods)
                if (m.written_name() == n)
                    return Member{RefKind::Method, d, ret_of(m), true};
            for (const Method& m : d->statics)
                if (m.written_name() == n)
                    return Member{RefKind::StaticMethod, d, ret_of(m), true};
            for (const Property& p : d->sprops)
                if (p.name == n)
                    return Member{RefKind::Property, d, p.script_type.empty() ? p.type : p.script_type, false};
            const std::string key = d->name + "__" + n;
            for (const Field& f : sc_.consts)
                if (f.owner == d->name && f.name == key)
                    return Member{RefKind::Const, d, type_of(f), false};
            for (const Field& f : sc_.vars)
                if (f.owner == d->name && f.name == key)
                    return Member{RefKind::StaticField, d, type_of(f), false};
            // A static field of an imported class stays in the module of the import.
            for (const Field* f : in_.foreign_vars)
                if (f && f->owner == d->name && f->name == key)
                    return Member{RefKind::StaticField, d, type_of(*f), false};
        }
        depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const InterfaceDef* in : ifaces_of(*d))
                if (const Method* m = iface_method(*in, n))
                    return Member{RefKind::Method, nullptr, ret_of(*m), true, in};
        return std::nullopt;
    }

    // The script interfaces in the base list of "c". The parser puts them in "interfaces" (the
    // translator moves them to "script_ifaces" after this pass).
    std::vector<const InterfaceDef*> ifaces_of(const ClassDef& c) const
    {
        std::vector<const InterfaceDef*> out;
        for (const auto* list : {&c.interfaces, &c.script_ifaces})
            for (const std::string& n : *list)
                if (auto it = ifaces_.find(plain_type(n)); it != ifaces_.end())
                    if (std::find(out.begin(), out.end(), it->second) == out.end())
                        out.push_back(it->second);
        return out;
    }

    static const Method* iface_method(const InterfaceDef& d, const std::string& n)
    {
        for (const Method& m : d.methods)
            if (m.name == n)
                return &m;
        return nullptr;
    }

    // The id ("decl" and file) of the instance method "n" of "c": the root declaration. The root
    // is the highest declaration in the class chain, and a method of a script interface of a
    // class in the chain is higher than the class. An override thus has the id of its root.
    std::pair<std::string, std::string> method_root(const ClassDef& c, const std::string& n) const
    {
        std::pair<std::string, std::string> root{c.name + "." + n, file_of(c.pos)};
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
        {
            for (const Method& m : d->methods)
                if (m.written_name() == n)
                {
                    root = {d->name + "." + n, file_of(d->pos)};
                    break;
                }
            for (const InterfaceDef* in : ifaces_of(*d))
                if (iface_method(*in, n))
                {
                    root = {in->name + "." + n, file_of(in->pos)};
                    break;
                }
        }
        return root;
    }

    const Local* local(const std::string& n) const
    {
        for (size_t i = scopes_.size(); i-- > 0;)
            if (auto it = scopes_[i].find(n); it != scopes_[i].end())
                return &it->second;
        return nullptr;
    }

    void declare(const std::string& name, uint32_t pos, RefKind kind, const std::string& type)
    {
        if (name.empty() || scopes_.empty())
            return;
        // The parser can give the offset of the blank before the name.
        if (pos && !word_at(pos, name))
            pos = find_word(pos, name, 16);
        scopes_.back()[name] = Local{pos, kind, type};
        if (pos)
            add(pos, name, kind, "@" + std::to_string(pos), std::string(), true);
    }

    void declare_params(const std::vector<Param>& params)
    {
        for (const Param& p : params)
        {
            if (p.type_pos && p.pos > p.type_pos)
                types_in(p.type_pos, p.pos);
            declare(p.name, p.pos, RefKind::Param, type_of(p));
            if (p.def)
                eval(p.def->expr);
        }
    }

    // ------------------------------------------------------------ the declarations
    void walk_class(const ClassDef& c)
    {
        cls_ = &c;
        const uint32_t name_pos = find_word(c.pos, c.name, 64);
        if (name_pos)
        {
            add(name_pos, c.name, RefKind::Class, c.name, std::string(), true);
            // The base list: from the name to the "{".
            const size_t open = in_.text.find('{', name_pos);
            if (open != std::string_view::npos)
                types_in(name_pos + static_cast<uint32_t>(c.name.size()), static_cast<uint32_t>(open));
        }
        for (const Field& f : c.fields)
        {
            const uint32_t p = find_word(f.pos, f.name, 64);
            type_before(p);
            add(p, f.name, RefKind::Field, c.name + "." + f.name, std::string(), true);
            if (!f.raw)
                body_expr(f.init);
            else
                add_raw(f.raw_span);
        }
        for (const Property& p : c.props)
            walk_prop(c, p);
        for (const Property& p : c.sprops)
            walk_prop(c, p);
        for (const Method& m : c.methods)
        {
            auto [decl, file] = method_root(c, m.written_name());
            walk_fn(m, &c, RefKind::Method, decl, file);
        }
        for (const Method& m : c.statics)
            walk_fn(m, &c, RefKind::StaticMethod, c.name + "." + m.written_name());
        for (const Ctor& k : c.ctors)
        {
            add_type(find_word(k.pos, c.name, 16), c.name);
            if (k.raw)
            {
                add_raw(k.raw_span);
                continue;
            }
            begin_body();
            declare_params(k.params);
            for (const ExprP& a : k.base_args)
                eval(a);
            block(k.body);
            end_body();
        }
        for (const Method& m : c.dtors)
        {
            add_type(find_word(m.pos, c.name, 16), c.name);
            if (m.raw)
            {
                add_raw(m.raw_span);
                continue;
            }
            begin_body();
            block(m.body);
            end_body();
        }
        cls_ = nullptr;
    }

    void walk_prop(const ClassDef& c, const Property& p)
    {
        const uint32_t at = find_word(p.pos, p.name, 64);
        type_before(at);
        add(at, p.name, RefKind::Property, c.name + "." + p.name, std::string(), true);
        body_expr(p.init);
        if (p.get_body)
        {
            begin_body();
            block(*p.get_body);
            end_body();
        }
        if (p.set_body)
        {
            begin_body();
            declare("value", 0, RefKind::Param, p.script_type.empty() ? p.type : p.script_type);
            block(*p.set_body);
            end_body();
        }
    }

    void walk_iface(const InterfaceDef& d)
    {
        decl_name(d.pos, d.name, RefKind::Interface, d.name, 64);
        for (const Method& m : d.methods)
        {
            const uint32_t at = find_word(m.pos, m.name, 64);
            type_before(at);
            add(at, m.name, RefKind::Method, d.name + "." + m.name, std::string(), true);
            for (const Param& p : m.params)
                if (p.type_pos && p.pos > p.type_pos)
                    types_in(p.type_pos, p.pos);
        }
    }

    void walk_static_field(const Field& f, RefKind kind)
    {
        std::string name = f.name;
        std::string decl = f.name;
        const ClassDef* owner = nullptr;
        if (!f.owner.empty())
        {
            if (name.rfind(f.owner + "__", 0) != 0)
                return;
            name = name.substr(f.owner.size() + 2);
            decl = f.owner + "." + name;
            owner = class_named(f.owner);
        }
        const uint32_t at = find_word(f.pos, name, 64);
        type_before(at);
        add(at, name, kind, decl, std::string(), true);
        const ClassDef* const outer = cls_;
        cls_ = owner;
        body_expr(f.init);
        cls_ = outer;
    }

    // "file": the file of the root declaration of an override (empty: the main script).
    void walk_fn(const Method& m, const ClassDef* c, RefKind kind, const std::string& decl,
        const std::string& file = std::string())
    {
        // walk_class calls this for each method and then walks the ctors and dtors with "cls_".
        const ClassDef* const outer = cls_;
        cls_ = c;
        const std::string& name = m.written_name();
        const uint32_t at = m.op.empty() ? find_word(m.pos, name, 64) : 0;
        type_before(at);
        add(at, name, kind, decl, file, true);
        if (!m.raw)
        {
            begin_body();
            declare_params(m.params);
            block(m.body);
            end_body();
        }
        else
            add_raw(m.raw_span);
        cls_ = outer;
    }

    void begin_body()
    {
        scopes_.clear();
        scopes_.emplace_back();
    }

    void end_body() { scopes_.clear(); }

    void body_expr(const ExprP& e)
    {
        if (!e)
            return;
        begin_body();
        eval(e);
        end_body();
    }

    // ------------------------------------------------------------ the names
    // The bare name "n" at "pos": a local, a member of the class, a module symbol or a type.
    Value head(const std::string& n, uint32_t pos)
    {
        Value v;
        if (const Local* l = local(n))
        {
            if (l->pos)
                add(pos, n, l->kind, "@" + std::to_string(l->pos), std::string(), false);
            v.type = l->type;
            return v;
        }
        if (cls_)
            if (std::optional<Member> m = member(*cls_, n))
            {
                add_member(pos, n, *m);
                (m->callable ? v.ret : v.type) = m->type;
                return v;
            }
        if (auto it = module_.find(n); it != module_.end())
        {
            add(pos, n, it->second.second, n, file_of(it->second.first->pos), false);
            v.type = type_of(*it->second.first);
            return v;
        }
        if (auto it = fns_.find(n); it != fns_.end())
        {
            add(pos, n, RefKind::Function, n, file_of(it->second->pos), false);
            v.ret = ret_of(*it->second);
            return v;
        }
        if (add_type(pos, n))
        {
            v.cls = class_named(n);
            if (auto it = enums_.find(n); it != enums_.end())
                v.en = it->second;
        }
        return v;
    }

    // The member "n" at "pos" of a value or of a scope.
    Value tail(const Value& of, const std::string& n, uint32_t pos)
    {
        Value v;
        if (of.en)
        {
            for (const Enumerator& it : of.en->items)
                if (it.name == n)
                {
                    add(pos, n, RefKind::EnumValue, of.en->name + "." + n, file_of(of.en->pos), false);
                    v.type = of.en->name;
                }
            return v;
        }
        const ClassDef* c = of.cls ? of.cls : class_named(of.type);
        if (!c)
        {
            // A receiver with the type of a script interface.
            auto it = ifaces_.find(plain_type(of.type));
            if (it == ifaces_.end())
            {
                // The receiver has no known script type: the name is perhaps native.
                add_unresolved(pos, n);
                return v;
            }
            if (const Method* m = iface_method(*it->second, n))
            {
                add_member(pos, n, Member{RefKind::Method, nullptr, ret_of(*m), true, it->second});
                v.ret = ret_of(*m);
            }
            return v;
        }
        if (std::optional<Member> m = member(*c, n))
        {
            add_member(pos, n, *m);
            (m->callable ? v.ret : v.type) = m->type;
        }
        return v;
    }

    Value name(const NameExpr& n)
    {
        exprs(n.targ_exprs);
        if (n.parts.empty())
            return {};
        auto part_pos = [&](size_t i) -> uint32_t
        {
            if (i < n.part_pos.size())
                return n.part_pos[i];
            if (i == 0)
                return n.pos;
            return i + 1 == n.parts.size() ? n.last_pos : 0;
        };
        Value v = head(n.parts[0], part_pos(0));
        for (size_t i = 1; i < n.parts.size(); ++i)
            v = tail(v, n.parts[i], part_pos(i));
        return v;
    }

    // ------------------------------------------------------------ the expressions
    void exprs(const std::vector<ExprP>& list)
    {
        for (const ExprP& e : list)
            eval(e);
    }

    Value eval(const ExprP& e) { return e ? eval(*e) : Value{}; }

    Value eval(const Expr& e)
    {
        return std::visit(
            Overloaded{
                [&](const NameExpr& n) { return name(n); },
                [&](const ThisExpr&)
                {
                    Value v;
                    if (cls_)
                        v.type = cls_->name;
                    return v;
                },
                [&](const BaseExpr&)
                {
                    Value v;
                    if (cls_)
                        v.type = cls_->base;
                    return v;
                },
                [&](const NullExpr&) { return Value{}; },
                [&](const NumberExpr&) { return Value{}; },
                [&](const StringExpr&) { return Value{}; },
                [&](const CharExpr&) { return Value{}; },
                [&](const MemberExpr& m)
                {
                    const Value of = eval(m.obj);
                    return m.name.empty() ? Value{} : tail(of, m.name, m.name_pos);
                },
                [&](const CallExpr& c)
                {
                    const Value f = eval(c.callee);
                    exprs(c.args);
                    Value v;
                    v.type = f.cls ? f.cls->name : f.ret;
                    return v;
                },
                [&](const IndexExpr& ix)
                {
                    eval(ix.obj);
                    eval(ix.index);
                    return Value{};
                },
                [&](const UnaryExpr& u)
                {
                    eval(u.operand);
                    return Value{};
                },
                [&](const BinaryExpr& b)
                {
                    eval(b.lhs);
                    eval(b.rhs);
                    return Value{};
                },
                [&](const TernaryExpr& t)
                {
                    eval(t.cond);
                    eval(t.a);
                    eval(t.b);
                    return Value{};
                },
                [&](const ParenExpr& p) { return eval(p.inner); },
                [&](const NewExpr& n)
                {
                    type_after(word_at(n.pos, "new") ? n.pos + 3 : n.pos);
                    exprs(n.args);
                    Value v;
                    v.type = n.type;
                    return v;
                },
                [&](const LambdaExpr& l)
                {
                    scopes_.emplace_back();
                    declare_params(l.params);
                    block(l.body);
                    scopes_.pop_back();
                    return Value{};
                },
                [&](const RawExpr&) { return Value{}; },
                [&](const TypeTestExpr& tt)
                {
                    eval(tt.obj);
                    const uint32_t limit = tt.bind_pos > tt.pos ? tt.bind_pos - tt.pos : 256;
                    if (const uint32_t w = find_word(tt.pos, tt.is_as ? "as" : "is", limit))
                        type_after(w + 2);
                    if (!tt.bind.empty())
                        declare(tt.bind, tt.bind_pos, RefKind::Local, tt.type);
                    Value v;
                    if (tt.is_as)
                        v.type = tt.type;
                    return v;
                },
                [&](const InterpExpr& ip)
                {
                    exprs(ip.args);
                    return Value{};
                },
                [&](const CollectionExpr& co)
                {
                    exprs(co.elems);
                    exprs(co.values);
                    return Value{};
                },
            },
            e.v);
    }

    // ------------------------------------------------------------ the statements
    void block(const BlockStmt& b)
    {
        scopes_.emplace_back();
        for (const StmtP& s : b.stmts)
            stmt(s);
        scopes_.pop_back();
    }

    void stmt(const StmtP& s)
    {
        if (s)
            stmt(*s);
    }

    void scoped(const StmtP& s)
    {
        scopes_.emplace_back();
        stmt(s);
        scopes_.pop_back();
    }

    void stmt(const Stmt& s)
    {
        std::visit(
            Overloaded{
                [&](const BlockStmt& b) { block(b); },
                [&](const ExprStmt& es) { eval(es.expr); },
                [&](const VarDeclStmt& d)
                {
                    const Value init = eval(d.init);
                    if (d.names.empty())
                    {
                        const uint32_t at = d.name_pos ? d.name_pos : find_word(d.pos, d.name, 256);
                        if (at > d.pos)
                            types_in(d.pos, at);
                        std::string type = plain_type(d.type) == "var" ? std::string() : d.type;
                        declare(d.name, at, RefKind::Local, type.empty() ? init.type : type);
                    }
                    else
                        for (size_t i = 0; i < d.names.size(); ++i)
                            declare(d.names[i], i < d.name_poss.size() ? d.name_poss[i] : 0, RefKind::Local, "");
                },
                [&](const IfStmt& i)
                {
                    scopes_.emplace_back();
                    eval(i.cond);
                    scoped(i.then);
                    scopes_.pop_back();
                    scoped(i.otherwise);
                },
                [&](const WhileStmt& w)
                {
                    scopes_.emplace_back();
                    eval(w.cond);
                    scoped(w.body);
                    scopes_.pop_back();
                },
                [&](const DoWhileStmt& dw)
                {
                    scoped(dw.body);
                    scopes_.emplace_back();
                    eval(dw.cond);
                    scopes_.pop_back();
                },
                [&](const ForStmt& f)
                {
                    scopes_.emplace_back();
                    for (const StmtP& x : f.init)
                        stmt(x);
                    eval(f.cond);
                    for (const ExprP& x : f.step)
                        eval(x);
                    scoped(f.body);
                    scopes_.pop_back();
                },
                [&](const ForOfStmt& fo)
                {
                    eval(fo.range);
                    scopes_.emplace_back();
                    if (fo.names.empty())
                    {
                        const uint32_t at = fo.name_pos ? fo.name_pos : find_word(fo.pos, fo.name, 256);
                        if (at > fo.pos)
                            types_in(fo.pos, at);
                        declare(fo.name, at, RefKind::Local, fo.type);
                    }
                    else
                        for (size_t i = 0; i < fo.names.size(); ++i)
                            declare(fo.names[i], i < fo.name_poss.size() ? fo.name_poss[i] : 0, RefKind::Local, "");
                    scoped(fo.body);
                    scopes_.pop_back();
                },
                [&](const ReturnStmt& r) { eval(r.expr); },
                [&](const BreakStmt&) {},
                [&](const ContinueStmt&) {},
                [&](const SwitchStmt& sw)
                {
                    eval(sw.subject);
                    scopes_.emplace_back();
                    for (const SwitchCase& c : sw.cases)
                    {
                        exprs(c.labels);
                        for (const StmtP& x : c.stmts)
                            stmt(x);
                    }
                    scopes_.pop_back();
                },
                [&](const ThrowStmt& t) { eval(t.expr); },
                [&](const TryStmt& tr)
                {
                    block(tr.block);
                    for (const CatchClause& c : tr.catches)
                    {
                        scopes_.emplace_back();
                        if (!c.is_all)
                            declare(c.name, find_word(c.pos, c.name, 256), RefKind::Local, c.type);
                        block(c.block);
                        scopes_.pop_back();
                    }
                    if (tr.finally_block)
                        block(*tr.finally_block);
                },
                [&](const EmptyStmt&) {},
                [&](const RawStmt&) {},
            },
            s.v);
    }
};

} // namespace

RefTable resolve_refs(const RefInputs& in)
{
    if (!in.script || !in.map)
        return {};
    return Resolver(in).run();
}

} // namespace hotc
