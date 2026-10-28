// The type model. See types.h.
#include "types.h"

#include <algorithm>
#include <deque>
#include <set>

namespace spc {

namespace {

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
    static const std::set<std::string> names = {
        "int",      "unsigned", "unsigned int", "long",     "short",    "float",    "double",   "bool",
        "char",     "void",     "auto",         "int8_t",   "int16_t",  "int32_t",  "int64_t",  "uint8_t",
        "uint16_t", "uint32_t", "uint64_t",     "size_t",   "ptrdiff_t", "intptr_t", "uintptr_t",
        "long long", "unsigned long", "unsigned long long", "unsigned char", "unsigned short",
        "signed char", "wchar_t", "char8_t", "char16_t", "char32_t"};
    return names.count(t) != 0;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to)
{
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

}  // namespace

void TypeModel::add_stubs(const std::vector<ast::Stub>& stubs, Diagnostics& diags, const LineMap& map)
{
    std::set<std::string> seen;  // the names in this list
    std::set<std::string> reported;
    for (const ast::Stub& s : stubs) {
        if (!s.default_handle.empty()) {
            diags.add(s.pos, map, "'default_handle' is not supported yet");
            continue;
        }
        if (s.name == "*") {
            diags.add(s.pos, map, "the stub 'class *' is not supported yet");
            continue;
        }
        if (!seen.insert(s.name).second) {
            if (reported.insert(s.name).second) diags.add(s.pos, map, "duplicate stub for '" + s.name + "'");
            continue;
        }

        const bool intrusive = !s.intrusive_add.empty() || !s.intrusive_release.empty();
        if (intrusive) {
            if (s.is_value) {
                diags.add(s.pos, map, "'value' and 'intrusive' are exclusive");
                continue;
            }
            if (!s.handle.empty() && s.handle != "$T*") {
                diags.add(s.pos, map, "'intrusive' needs handle(\"$T*\")");
                continue;
            }
        }

        TypeRule rule;
        if (intrusive) {
            rule.kind = Kind::Handle;
            rule.handle_tpl = "$T*";
            rule.intrusive_add = s.intrusive_add;
            rule.intrusive_release = s.intrusive_release;
        } else if (!s.handle.empty()) {
            rule.kind = Kind::Handle;
            rule.handle_tpl = s.handle;
        } else if (s.is_value) {
            rule.kind = Kind::Value;
        }

        const bool pattern = ends_with(s.name, "<*>") || ends_with(s.name, "::*");
        if (!pattern) {
            exact_[s.name] = rule;
            continue;
        }
        // "std::vector<*>" gives "std::vector<". "physics::*" gives "physics::".
        const std::string prefix = s.name.substr(0, s.name.size() - (ends_with(s.name, "<*>") ? 2 : 1));
        auto it = std::find_if(patterns_.begin(), patterns_.end(),
                               [&](const auto& p) { return p.first == prefix; });
        if (it != patterns_.end()) it->second = rule;
        else patterns_.emplace_back(prefix, rule);
    }
}

void TypeModel::set_bases(BaseFn fn)
{
    bases_ = std::move(fn);
}

TypeRule TypeModel::lookup(const std::string& cpp_type) const
{
    auto it = exact_.find(cpp_type);
    if (it != exact_.end()) {
        TypeRule r = it->second;
        r.owner = cpp_type;
        return r;
    }

    if (bases_) {
        // Breadth first walk of the bases. The visited set stops a cycle.
        std::set<std::string> visited{cpp_type};
        std::deque<std::string> queue{cpp_type};
        while (!queue.empty()) {
            const std::string current = queue.front();
            queue.pop_front();
            for (const std::string& base : bases_(current)) {
                if (!visited.insert(base).second) continue;
                auto b = exact_.find(base);
                if (b != exact_.end() && b->second.kind == Kind::Handle) {
                    TypeRule r = b->second;
                    r.owner = base;
                    return r;
                }
                queue.push_back(base);
            }
        }
    }

    const std::pair<std::string, TypeRule>* best = nullptr;
    for (const auto& p : patterns_)
        if (starts_with(cpp_type, p.first) && (!best || p.first.size() > best->first.size())) best = &p;
    if (!best) return TypeRule{};
    TypeRule r = best->second;
    // Rebuild the pattern name from the prefix.
    r.owner = best->first + (ends_with(best->first, "<") ? "*>" : "*");
    return r;
}

bool TypeModel::is_handle(const std::string& type) const
{
    if (type.find('*') != std::string::npos || type.find('&') != std::string::npos) return false;
    if (starts_with(type, "const ") || is_builtin(type)) return false;
    return lookup(type).kind == Kind::Handle;
}

std::string TypeModel::storage(const std::string& type) const
{
    if (!is_handle(type)) return type;
    const TypeRule rule = lookup(type);
    if (!rule.intrusive_add.empty())
        return "script::Intrusive<" + type + ", &" + rule.owner + "::" + rule.intrusive_add + ", &" +
               rule.owner + "::" + rule.intrusive_release + ">";
    return replace_all(rule.handle_tpl, "$T", type);
}

std::string TypeModel::param(const std::string& type) const
{
    if (!is_handle(type)) return type;
    return "const " + storage(type) + "&";
}

std::string TypeModel::ref_param(const std::string& type) const { return storage(type) + "&"; }

std::string TypeModel::make(const std::string& type, const std::string& args) const
{
    if (!is_handle(type)) return type + "(" + args + ")";
    if (lookup(type).handle_tpl == "std::shared_ptr<$T>") return "std::make_shared<" + type + ">(" + args + ")";
    return storage(type) + "(new " + type + "(" + args + "))";
}

TypeModel::Callee TypeModel::make_callee(const std::string& type) const
{
    if (!is_handle(type)) return {type, false};
    if (lookup(type).handle_tpl == "std::shared_ptr<$T>") return {"std::make_shared<" + type + ">", false};
    return {storage(type), true};
}

}  // namespace spc
