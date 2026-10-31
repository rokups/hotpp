// The type model. See types.h.
#include "types.h"

#include "builtin_types.h"

#include <algorithm>
#include <deque>
#include <set>

namespace hotc
{

namespace
{

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool is_builtin(const std::string& t)
{
    static const std::set<std::string> names = {"int", "unsigned", "unsigned int", "long", "short", "float", "double",
        "bool", "char", "void", "auto", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "size_t", "ptrdiff_t", "intptr_t", "uintptr_t", "long long", "unsigned long", "unsigned long long",
        "unsigned char", "unsigned short", "signed char", "wchar_t", "char8_t", "char16_t", "char32_t"};
    return names.count(t) != 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to)
{
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
    return s;
}

} // namespace

void TypeModel::add_stubs(const std::vector<ast::Stub>& stubs, Diagnostics& diags, const LineMap& map, Source source)
{
    std::set<std::string> seen; // the names in this list
    std::set<std::string> reported;
    for (const ast::Stub& s : stubs)
    {
        // The key of the stub in this list: "class : B" and "struct *" have own keys.
        const std::string key = s.is_struct ? "struct *" : s.derived ? ": " + s.name : s.name;
        if (!seen.insert(key).second)
        {
            if (reported.insert(key).second)
            {
                diags.add(s.pos, map,
                    s.is_struct       ? "only one 'struct *' is allowed in a file"
                        : s.name == "*" && !s.derived ? "only one 'class *' is allowed in a file"
                                                      : "duplicate stub for '" + key + "'");
            }
            continue;
        }
        if (s.is_struct)
        {
            struct_prelude_ = s.cpp_prelude;
            continue;
        }
        if (s.derived && s.is_value)
        {
            diags.add(s.pos, map, "'value' is not allowed on 'class : Base': it breaks polymorphic types");
            continue;
        }
        if (s.name == "*" && !s.derived)
        {
            if (s.is_value)
                diags.add(s.pos, map, "'value' is not allowed on 'class *': it breaks polymorphic types");
            else if (!s.handle.empty() || !s.intrusive_add.empty() || !s.intrusive_release.empty())
                diags.add(s.pos, map, "'class *' accepts only 'native' and 'default_handle'");
            else
            {
                // A later list overrides an earlier "class *". A stub without a handle clears it.
                if (source == Source::Script && !default_handle_.empty())
                    diags.warn(s.pos, map, "the 'class *' of the script overrides the 'class *' of a prelude");
                default_handle_ = s.default_handle;
                class_prelude_ = s.cpp_prelude;
            }
            continue;
        }
        if (!s.default_handle.empty())
        {
            diags.add(s.pos, map, "'default_handle' is valid only on 'class *'");
            continue;
        }
        if (s.is_value && !s.handle.empty())
        {
            diags.add(s.pos, map, "'value' and 'handle' are exclusive");
            continue;
        }

        const bool intrusive = !s.intrusive_add.empty() || !s.intrusive_release.empty();
        if (intrusive)
        {
            if (s.is_value)
            {
                diags.add(s.pos, map, "'value' and 'intrusive' are exclusive");
                continue;
            }
            if (!s.handle.empty() && s.handle != "$T*")
            {
                diags.add(s.pos, map, "'intrusive' needs handle(\"$T*\")");
                continue;
            }
        }

        TypeRule rule;
        rule.self = s.is_self;
        rule.native = s.is_native;
        rule.cpp_prelude = s.cpp_prelude;
        if (intrusive)
        {
            rule.kind = Kind::Handle;
            rule.handle_tpl = "$T*";
            rule.intrusive_add = s.intrusive_add;
            rule.intrusive_release = s.intrusive_release;
        }
        else if (!s.handle.empty())
        {
            rule.kind = Kind::Handle;
            rule.handle_tpl = s.handle;
        }
        else if (s.is_value)
        {
            rule.kind = Kind::Value;
        }

        // A script stub that replaces a stub of a prelude gives a warning.
        if (source == Source::Prelude)
            prelude_names_.insert(key);
        else if (prelude_names_.count(key))
            diags.warn(s.pos, map, "the stub for '" + key + "' overrides the stub of a prelude");

        if (s.derived)
        {
            derived_[s.name] = rule;
            continue;
        }

        const bool pattern = ends_with(s.name, "<*>") || ends_with(s.name, "::*");
        if (!pattern)
        {
            exact_[s.name] = rule;
            continue;
        }
        // "std::vector<*>" gives "std::vector<". "physics::*" gives "physics::".
        const std::string prefix = s.name.substr(0, s.name.size() - (ends_with(s.name, "<*>") ? 2 : 1));
        auto it = std::find_if(patterns_.begin(), patterns_.end(), [&](const auto& p) { return p.first == prefix; });
        if (it != patterns_.end())
            it->second = rule;
        else
            patterns_.emplace_back(prefix, rule);
    }
}

void TypeModel::add_script_classes(const std::vector<ast::ClassDef>& classes)
{
    script_.clear();
    for (const ast::ClassDef& cd : classes)
        script_[cd.name] = {cd.base, cd.interfaces, cd.is_value, cd.cpp_prelude};
}

bool TypeModel::is_script_value(const std::string& type) const
{
    auto it = script_.find(type);
    return it != script_.end() && it->second.is_value;
}

void TypeModel::set_bases(BaseFn fn)
{
    bases_ = std::move(fn);
}

void TypeModel::set_classes(ClassFn fn)
{
    classes_ = std::move(fn);
}

bool TypeModel::wants_facts(const std::string& type) const
{
    if (type.empty())
        return false;
    if (type.find('*') != std::string::npos || type.find('&') != std::string::npos)
        return false;
    if (starts_with(type, "const ") || is_builtin(type))
        return false;
    return lookup_stub(type).kind == Kind::Raw;
}

TypeRule TypeModel::lookup(const std::string& cpp_type) const
{
    TypeRule r = lookup_stub(cpp_type);
    if (r.kind != Kind::Raw || !r.owner.empty() || !classes_ || !wants_facts(cpp_type))
        return r;
    const ClassFacts f = classes_(cpp_type);
    if (!f.known)
        return r;
    TypeRule d;
    d.owner = "*";
    if (f.plain)
    {
        d.kind = Kind::Value; // rule 4 of 7.10.2: plain data is a value (no stub needed)
        return d;
    }
    if (default_handle_.empty())
        return r; // no "class *;" stub: the type stays raw
    d.kind = Kind::Handle;
    d.handle_tpl = default_handle_;
    return d;
}

std::vector<std::string> TypeModel::direct_bases(const std::string& name) const
{
    auto sc = script_.find(name);
    if (sc != script_.end())
    {
        std::vector<std::string> out;
        if (!sc->second.base.empty())
            out.push_back(sc->second.base);
        out.insert(out.end(), sc->second.interfaces.begin(), sc->second.interfaces.end());
        return out;
    }
    return bases_ ? bases_(name) : std::vector<std::string>{};
}

bool TypeModel::base_rule(const std::string& base, TypeRule& out) const
{
    auto d = derived_.find(base);
    // A rule "class : B;" with only a cpp_prelude is not a storage rule: the walk goes on.
    if (d != derived_.end() && (d->second.kind != Kind::Raw || d->second.native))
    {
        out = d->second;
        out.owner = base;
        return true;
    }
    auto b = exact_.find(base);
    if (b != exact_.end() && b->second.kind == Kind::Handle && !b->second.self)
    {
        out = b->second;
        out.owner = base;
        return true;
    }
    return false;
}

TypeRule TypeModel::lookup_stub(const std::string& cpp_type) const
{
    // Breadth first walk of the bases. The visited set stops a cycle.
    auto walk = [&](TypeRule& out)
    {
        std::set<std::string> visited{cpp_type};
        std::deque<std::string> queue{cpp_type};
        while (!queue.empty())
        {
            const std::string current = queue.front();
            queue.pop_front();
            for (const std::string& base : direct_bases(current))
            {
                if (!visited.insert(base).second)
                    continue;
                if (base_rule(base, out))
                    return true;
                queue.push_back(base);
            }
        }
        return false;
    };

    auto sc = script_.find(cpp_type);
    if (sc != script_.end())
    {
        TypeRule r;
        r.owner = cpp_type;
        if (sc->second.is_value)
        {
            r.kind = Kind::Value;
            return r;
        }
        // The handle of the native base, else the default handle of a script class.
        TypeRule found;
        if (walk(found))
        {
            if (found.kind == Kind::Handle)
            {
                found.cpp_prelude.clear();
                found.self = false;
                return found;
            }
        }
        else if (!sc->second.base.empty())
        {
            // A pattern, the facts or "class *" of the base. The exact stub of a native base
            // was in the walk: a stub with [self] is not for a subclass.
            const TypeRule b = lookup(sc->second.base);
            if (b.kind == Kind::Handle && (b.owner != sc->second.base || script_.count(b.owner)))
            {
                r = b;
                return r;
            }
        }
        r.kind = Kind::Handle;
        r.handle_tpl = "std::shared_ptr<$T>";
        return r;
    }
    auto it = exact_.find(cpp_type);
    if (it != exact_.end())
    {
        TypeRule r = it->second;
        r.owner = cpp_type;
        return r;
    }

    {
        TypeRule r;
        if (walk(r))
            return r;
    }

    const std::pair<std::string, TypeRule>* best = nullptr;
    for (const auto& p : patterns_)
        if (starts_with(cpp_type, p.first) && (!best || p.first.size() > best->first.size()))
            best = &p;
    if (!best)
        return TypeRule{};
    TypeRule r = best->second;
    // Rebuild the pattern name from the prefix.
    r.owner = best->first + (ends_with(best->first, "<") ? "*>" : "*");
    return r;
}

bool TypeModel::is_handle(const std::string& type) const
{
    if (type.find('*') != std::string::npos || type.find('&') != std::string::npos)
        return false;
    if (starts_with(type, "const ") || is_builtin(type))
        return false;
    return lookup(type).kind == Kind::Handle;
}

bool TypeModel::has_shared_from_this(const std::string& type) const
{
    std::set<std::string> visited;
    std::deque<std::string> queue{type};
    while (!queue.empty())
    {
        const std::string current = queue.front();
        queue.pop_front();
        if (!visited.insert(current).second)
            continue;
        if (classes_ && classes_(current).shared_from_this)
            return true;
        for (const std::string& base : direct_bases(current))
            queue.push_back(base);
    }
    return false;
}

bool TypeModel::is_script_class(const std::string& type) const
{
    return script_.count(type) != 0;
}

std::string TypeModel::script_ref_storage(const std::string& type) const
{
    size_t e = type.find_last_not_of(" \t");
    if (raw_script_refs_ || e == std::string::npos || type[e] != '&' || (e > 0 && type[e - 1] == '&'))
        return {};
    std::string t = type.substr(0, e);
    bool is_const = false;
    if (starts_with(t, "const "))
    {
        is_const = true;
        t = t.substr(6);
    }
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    if (!t.empty() && t.back() == '*')
        t.pop_back(); // "T*&": the script class is a handle
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
        t.pop_back();
    if (t.empty() || !is_script_class(t) || is_script_value(t) || !is_handle(t))
        return {};
    return std::string(is_const ? "const " : "") + storage(t) + "&";
}

std::string TypeModel::storage(const std::string& type) const
{
    if (const std::string r = script_ref_storage(type); !r.empty())
        return r;
    if (!is_handle(type))
        return type;
    const TypeRule rule = lookup(type);
    if (!rule.intrusive_add.empty())
        return "hot::Intrusive<" + type + ", &" + rule.owner + "::" + rule.intrusive_add + ", &" + rule.owner
            + "::" + rule.intrusive_release + ">";
    return replace_all(rule.handle_tpl, "$T", type);
}

std::string TypeModel::param(const std::string& type) const
{
    if (const std::string r = script_ref_storage(type); !r.empty())
        return r;
    if (!is_handle(type))
        return type;
    return "const " + storage(type) + "&";
}

std::string TypeModel::body_param(const std::string& type, bool mutated, bool rebound, bool stored) const
{
    if (type.empty() || type.find('*') != std::string::npos || type.find('&') != std::string::npos
        || starts_with(type, "const ") || is_builtin(type))
        return param(type);
    const TypeRule rule = lookup(type);
    if (rule.kind == Kind::Handle)
        return rebound || stored ? storage(type) : param(type);
    if (rule.kind != Kind::Value)
        return type;
    if (mutated || stored)
        return type;
    if (classes_ && classes_(type).small)
        return type;
    return "const " + type + "&";
}

std::string TypeModel::make(const std::string& type, const std::string& args) const
{
    if (!is_handle(type))
        return type + "(" + args + ")";
    if (lookup(type).handle_tpl == "std::shared_ptr<$T>")
        return "std::make_shared<" + type + ">(" + args + ")";
    return storage(type) + "(new " + type + "(" + args + "))";
}

TypeModel::Callee TypeModel::make_callee(const std::string& type) const
{
    if (!is_handle(type))
        return {type, false};
    if (lookup(type).handle_tpl == "std::shared_ptr<$T>")
        return {"std::make_shared<" + type + ">", false};
    return {storage(type), true};
}


std::string override_key_norm(const std::string& type)
{
    std::string r;
    for (char c : canonical_type(type, nullptr, true))
        if (!std::isspace(static_cast<unsigned char>(c)))
            r += c;
    return r;
}

std::string TypeModel::carrier_of(const std::string& native_param_type) const
{
    // Work on the canonical text with single spaces.
    std::string t = canonical_type(native_param_type, nullptr, true);
    auto trim = [](std::string& x)
    {
        const size_t b = x.find_first_not_of(" \t");
        const size_t e = x.find_last_not_of(" \t");
        x = b == std::string::npos ? std::string() : x.substr(b, e - b + 1);
    };
    trim(t);
    bool is_const = false;
    if (starts_with(t, "const ") || starts_with(t, "const\t"))
    {
        is_const = true;
        t = t.substr(6);
        trim(t);
    }
    enum class Suffix
    {
        None,
        Ptr,
        Ref,
        RRef
    } suffix = Suffix::None;
    if (ends_with(t, "&&"))
    {
        suffix = Suffix::RRef;
        t.resize(t.size() - 2);
    }
    else if (ends_with(t, "&"))
    {
        suffix = Suffix::Ref;
        t.resize(t.size() - 1);
    }
    else if (ends_with(t, "*"))
    {
        suffix = Suffix::Ptr;
        t.resize(t.size() - 1);
    }
    trim(t);
    std::string core;
    for (char c : t)
        if (!std::isspace(static_cast<unsigned char>(c)))
            core += c;
    if (core.empty() || core.find('*') != std::string::npos || core.find('&') != std::string::npos)
        return {};

    // "T*", "const T*", "T&" and "const T&".
    if ((suffix == Suffix::Ptr || suffix == Suffix::Ref) && is_handle(core))
        return core;

    // The storage text of a handle type: "$T*" of an intrusive stub is "T*".
    auto native_storage = [&](const std::string& type)
    {
        const TypeRule rule = lookup(type);
        return replace_all(rule.handle_tpl, "$T", type);
    };
    auto strip = [](const std::string& x)
    {
        std::string r;
        for (char c : x)
            if (!std::isspace(static_cast<unsigned char>(c)))
                r += c;
        return r;
    };

    // "H" (by value), "const H&" and "H&&". The templates tell where T is in the text of H.
    // "H&" without const is not a carrier.
    const bool h_form = (suffix == Suffix::None && !is_const) || (suffix == Suffix::Ref && is_const)
        || (suffix == Suffix::RRef && !is_const);
    if (!h_form)
        return {};
    std::set<std::string> templates;
    auto add_tpl = [&](const std::string& tpl)
    {
        if (tpl.find("$T") != std::string::npos)
            templates.insert(strip(tpl));
    };
    for (const auto& e : exact_)
        add_tpl(e.second.handle_tpl);
    for (const auto& e : derived_)
        add_tpl(e.second.handle_tpl);
    for (const auto& e : patterns_)
        add_tpl(e.second.handle_tpl);
    add_tpl(default_handle_);
    add_tpl("std::shared_ptr<$T>");
    for (const std::string& tpl : templates)
    {
        const size_t at = tpl.find("$T");
        const std::string pre = tpl.substr(0, at), post = tpl.substr(at + 2);
        if (core.size() <= pre.size() + post.size() || !starts_with(core, pre) || !ends_with(core, post))
            continue;
        const std::string inner = core.substr(pre.size(), core.size() - pre.size() - post.size());
        if (is_handle(inner) && strip(native_storage(inner)) == core)
            return inner;
    }
    return {};
}

std::string TypeModel::return_carrier_of(const std::string& native_return_type) const
{
    std::string t = native_return_type;
    const size_t e = t.find_last_not_of(" \t");
    if (e == std::string::npos || t[e] == '*' || t[e] == '&')
        return {};
    return carrier_of(t);
}

bool override_return_ok(
    const std::string& script_ret, const std::string& native_ret, const TypeModel& model, bool relaxed)
{
    const std::string s = override_key_norm(script_ret);
    if (s == override_key_norm(native_ret))
        return true;
    if (!relaxed)
        return false;
    const std::string t = model.return_carrier_of(native_ret);
    return !t.empty() && override_key_norm(t) == s;
}

OverrideMatch match_native_virtual(const std::string& name, const std::vector<std::string>& script_ptypes,
    const std::vector<VirtualSig>& virtuals, const TypeModel& model, bool is_const, bool relaxed)
{
    OverrideMatch result;
    std::string key;
    for (const std::string& p : script_ptypes)
        key += override_key_norm(p) + ",";
    auto native_key = [&](const VirtualSig& v, bool use_carriers)
    {
        std::string k;
        for (const std::string& p : v.ptypes)
        {
            std::string c = use_carriers ? model.carrier_of(p) : std::string();
            k += (c.empty() ? override_key_norm(p) : override_key_norm(c)) + ",";
        }
        return k;
    };
    auto collect = [&](bool use_carriers)
    {
        std::vector<size_t> matches;
        for (size_t i = 0; i < virtuals.size(); ++i)
            if (virtuals[i].name == name && native_key(virtuals[i], use_carriers) == key)
                matches.push_back(i);
        return matches;
    };
    auto select = [&](const std::vector<size_t>& matches)
    {
        std::vector<size_t> same_const;
        for (size_t i : matches)
            if (virtuals[i].is_const == is_const)
                same_const.push_back(i);
        if (same_const.empty() && is_const)
        {
            result.kind = OverrideMatch::Kind::ConstMismatch;
            result.hits = matches;
            return;
        }
        // Without "const" and without a non-const match, use the const matches.
        result.hits = same_const.empty() ? matches : same_const;
        result.kind = result.hits.size() > 1 ? OverrideMatch::Kind::Ambiguous : OverrideMatch::Kind::Hit;
    };

    std::vector<size_t> exact = collect(false);
    if (!exact.empty())
    {
        select(exact);
        return result;
    }
    std::vector<size_t> loose = collect(true);
    if (loose.empty())
        return result;
    if (relaxed)
    {
        select(loose);
        result.relaxed = true;
        return result;
    }
    result.relaxed_candidates = loose;
    return result;
}

std::vector<std::string> TypeModel::preludes(const std::string& type) const
{
    std::vector<std::string> out;
    auto sc = script_.find(type);
    if (sc == script_.end())
        return out;
    auto add = [&](const std::string& text)
    {
        if (!text.empty())
            out.push_back(text);
    };
    add(sc->second.is_value ? struct_prelude_ : class_prelude_);

    // All bases, nearest first. The list is reversed after: the farthest base comes first.
    std::vector<std::string> order;
    std::set<std::string> visited{type};
    std::deque<std::string> queue{type};
    while (!queue.empty())
    {
        const std::string current = queue.front();
        queue.pop_front();
        for (const std::string& base : direct_bases(current))
            if (visited.insert(base).second)
            {
                order.push_back(base);
                queue.push_back(base);
            }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        auto e = exact_.find(*it);
        if (e != exact_.end() && !e->second.self)
            add(e->second.cpp_prelude);
        auto d = derived_.find(*it);
        if (d != derived_.end())
            add(d->second.cpp_prelude);
    }
    add(sc->second.cpp_prelude);
    return out;
}

} // namespace hotc
