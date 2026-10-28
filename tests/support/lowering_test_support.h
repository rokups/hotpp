// Helpers for the tests of the lowering pass. The fixture is the class "Player : Actor".
#pragma once

#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "emit.h"
#include "lower.h"
#include "parser/expression.h"
#include "parser/parser.h"
#include "parser/statement.h"
#include "types.h"

namespace lowering_test {

using namespace spc;

// The class "Player : Actor" with these members.
//   fields: health float, fired int, n int, target Actor*
//   fns:    heal
struct Fixture {
    ast::ClassDef cd;
    RCtx cx;
    Diagnostics diag;

    Fixture()
    {
        cd.name = "Player";
        cd.base = "Actor";
        auto field = [&](const char* name, const char* type) {
            ast::Field f;
            f.name = name;
            f.type = type;
            cd.fields.push_back(std::move(f));
        };
        field("health", "float");
        field("fired", "int");
        field("n", "int");
        field("target", "Actor*");
        ast::Method heal;
        heal.name = "heal";
        cd.methods.push_back(std::move(heal));
        cx = make_class_ctx(cd);
        cx.where = "Player::run";
    }

    // The errors as "line:col: text".
    std::vector<std::string> errors() const
    {
        std::vector<std::string> r;
        for (const Diag& d : diag.items)
            r.push_back(std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.msg);
        return r;
    }

    // Lowers the text as the statements of a body and prints them, separated by one space.
    std::string body(const std::string& text)
    {
        LineMap map(text);
        cx.map = &map;
        ast::BlockStmt b = parse_block({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
        lower_block(b, cx, diag);
        cx.map = nullptr;
        std::string r;
        for (const ast::StmtP& s : b.stmts) r += (r.empty() ? "" : " ") + ast::print(*s);
        return r;
    }

    std::string expr(const std::string& text)
    {
        LineMap map(text);
        cx.map = &map;
        ast::ExprP e = parse_expression({text, &map}, {0, static_cast<uint32_t>(text.size())}, diag);
        EXPECT_TRUE(e) << text;
        if (!e) return "";
        lower_expr(e, cx, diag);
        cx.map = nullptr;
        return ast::print(*e);
    }

    void param(const char* name) { cx.locals.insert(name); }
};

// A type model with one stub. The handle is empty for a value stub.
inline TypeModel stub_model(const char* name, const char* handle, bool is_value = false)
{
    std::vector<ast::Stub> stubs(1);
    stubs[0].name = name;
    stubs[0].is_native = true;
    stubs[0].handle = handle;
    stubs[0].is_value = is_value;
    LineMap smap("");
    Diagnostics sdiag;
    TypeModel types;
    types.add_stubs(stubs, sdiag, smap);
    EXPECT_TRUE(sdiag.items.empty());
    return types;
}

// The fixture class with the properties Hp (get and set), Max (get only) and Sink (set only).
inline void add_props(Fixture& f)
{
    auto prop = [&](const char* name, bool get, bool set) {
        ast::Property p;
        p.name = name;
        p.type = "float";
        p.has_get = get;
        p.has_set = set;
        f.cd.props.push_back(std::move(p));
    };
    prop("Hp", true, true);
    prop("Max", true, false);
    prop("Sink", false, true);
    f.cx = make_class_ctx(f.cd);
}

}  // namespace lowering_test
