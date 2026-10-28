// Helpers for the tests of the script parser: classes, members, modifiers and stubs.
#pragma once

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "parser/parser.h"

namespace script_test {

using namespace spc;

// Parses a script. The value is the parameters of the first method, as "type name" items.
inline std::vector<std::string> method_params(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    std::vector<std::string> out;
    if (script.classes.empty() || script.classes[0].methods.empty()) return out;
    for (const ast::Param& p : script.classes[0].methods[0].params)
        out.push_back(p.type + "|" + p.name);
    return out;
}

// Parses a class. The value lists the members: "method ret|name" and "field type|name|init".
inline std::vector<std::string> members_of(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    std::vector<std::string> out;
    if (script.classes.empty()) return out;
    for (const ast::Field& f : script.classes[0].fields)
        out.push_back("field " + f.type + "|" + f.name + "|" + (f.has_init ? "init" : "-") +
                      "|" + f.alias);
    for (const ast::Method& m : script.classes[0].methods)
        out.push_back("method " + m.ret + "|" + m.name);
    return out;
}

// Parses a class. The value lists the properties: "type|name|get|set|init" with an access
// letter and "auto" or "body" for each accessor.
inline std::vector<std::string> props_of(const std::string& text, Diagnostics& sd)
{
    const LineMap map(text);
    const ast::Script script = parse_script(text, map, sd);
    std::vector<std::string> out;
    if (script.classes.empty()) return out;
    auto acc = [](ast::Access a) {
        return a == ast::Access::Public ? "u" : a == ast::Access::Private ? "r" : "o";
    };
    for (const ast::Property& p : script.classes[0].props) {
        std::string s = p.type + "|" + p.name + "|" + acc(p.access);
        s += p.has_get ? std::string("|get:") + acc(p.get_access) + (p.get_body ? ":body" : ":auto") : "|-";
        s += p.has_set ? std::string("|set:") + acc(p.set_access) + (p.set_body ? ":body" : ":auto") : "|-";
        s += p.has_init ? (p.init ? "|init" : "|bad-init") : "|-";
        if (p.get_body) s += "|g" + std::to_string(p.get_body->stmts.size());
        if (p.set_body) s += "|s" + std::to_string(p.set_body->stmts.size());
        out.push_back(s);
    }
    return out;
}

// Parses a class. The value is the first method, or null when there is none.
struct ModParsed {
    std::string text;
    LineMap map;
    Diagnostics sd;
    ast::Script script;

    explicit ModParsed(std::string t) : text(std::move(t)), map(text)
    {
        script = parse_script(text, map, sd);
    }
    const ast::Method& method(size_t i = 0) const { return script.classes.at(0).methods.at(i); }
    const ast::Field& field(size_t i = 0) const { return script.classes.at(0).fields.at(i); }
    // The errors as "line:col: text", one in each item.
    std::vector<std::string> errors() const
    {
        std::vector<std::string> r;
        for (const Diag& d : sd.items)
            r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
        return r;
    }
};

inline std::string member_class(const std::string& member)
{
    return "class A : B {\n    " + member + "\n}\n";
}

struct StubParsed {
    std::string text;
    LineMap map;
    Diagnostics diag;
    ast::Script script;

    explicit StubParsed(std::string t) : text(std::move(t)), map(text)
    {
        script = parse_script(text, map, diag);
    }
};

}  // namespace script_test
