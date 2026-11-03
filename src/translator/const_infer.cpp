// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The const inference. See const_infer.h for the rules.
#include "const_infer.h"

#include "ast_walk.h"
#include "builtin_types.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace hotc
{

namespace
{

using namespace ast;

bool is_assign_op(const std::string& op)
{
    return op == "=" || (op.size() >= 2 && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=");
}

// The name at the root of an access path. "exact" is true when the path is the bare name
// (maybe in parentheses). A path through "*", "." or "[]" is not exact. A cast to a reference type
// keeps the path. Any other cast makes a value, so it has no root.
struct Root
{
    const std::string* name = nullptr;
    bool exact = false;
};

Root root_of(const Expr& e, bool this_paths = false)
{
    if (auto* n = std::get_if<NameExpr>(&e.v))
    {
        if (n->parts.empty())
            return {};
        return {&n->parts[0], n->parts.size() == 1};
    }
    if (auto* m = std::get_if<MemberExpr>(&e.v))
    {
        // "this.f": with "this_paths" the root is the field "f".
        if (this_paths && m->obj && std::holds_alternative<ThisExpr>(m->obj->v))
            return {&m->name, true};
        Root r = m->obj ? root_of(*m->obj, this_paths) : Root{};
        r.exact = false;
        return r;
    }
    if (auto* ix = std::get_if<IndexExpr>(&e.v))
    {
        Root r = ix->obj ? root_of(*ix->obj, this_paths) : Root{};
        r.exact = false;
        return r;
    }
    if (auto* p = std::get_if<ParenExpr>(&e.v))
        return p->inner ? root_of(*p->inner, this_paths) : Root{};
    if (auto* u = std::get_if<UnaryExpr>(&e.v))
    {
        if (u->postfix || !u->operand)
            return {};
        const bool ref_cast = u->op.size() > 2 && u->op.front() == '(' && u->op.compare(u->op.size() - 2, 2, "&)") == 0;
        if (u->op == "*" || ref_cast)
        {
            Root r = root_of(*u->operand, this_paths);
            r.exact = false;
            return r;
        }
    }
    return {};
}

bool is_literal_scalar(const Expr& e)
{
    if (std::holds_alternative<NumberExpr>(e.v) || std::holds_alternative<CharExpr>(e.v))
        return true;
    if (auto* n = std::get_if<NameExpr>(&e.v))
        return n->parts.size() == 1 && (n->parts[0] == "true" || n->parts[0] == "false");
    return false;
}

bool is_builtin_scalar(const std::string& type)
{
    if (type.empty())
        return false;
    // The parser writes the C++ name ("int32_t"). The script name is also accepted.
    for (const BuiltinType& b : builtin_types())
        if (type == b.cpp || type == b.script || (b.alias && type == b.alias))
            return true;
    return false;
}

// True for the type "T[]" (a view of mutable elements). A "const T[]" and a "std::span<const T>"
// view cannot write. The parser can write the script text or the C++ text of the type.
bool is_mutable_view(const std::string& type)
{
    if (type.size() >= 2 && type.compare(type.size() - 2, 2, "[]") == 0)
        return type.compare(0, 6, "const ") != 0;
    return type.compare(0, 10, "std::span<") == 0 && type.compare(0, 16, "std::span<const ") != 0;
}

// What the pass knows about the names of one function body.
struct BodyInfo
{
    const ConstContext& ctx;
    std::set<std::string> declared;
    std::map<std::string, bool> handle; // true when each declaration of the name is a handle
    std::map<std::string, bool> copy_ok; // true when "return name;" copies at no risk
    std::map<std::string, bool> scalar; // true when each declaration of the name is a number
    std::set<std::string> written;

    void declare(const std::string& name, bool is_handle, bool is_copy_ok)
    {
        declared.insert(name);
        auto h = handle.emplace(name, is_handle);
        if (!h.second)
            h.first->second = h.first->second && is_handle;
        auto c = copy_ok.emplace(name, is_copy_ok);
        if (!c.second)
            c.first->second = c.first->second && is_copy_ok;
    }

    bool handle_type(const std::string& type, bool value) const
    {
        return !type.empty() && !value && ctx.is_handle && ctx.is_handle(type);
    }

    void declare_typed(const std::string& name, const std::string& type, bool value, const Expr* init)
    {
        const bool h = handle_type(type, value);
        const bool is_scalar = type.empty() ? init && is_literal_scalar(*init) : is_builtin_scalar(type);
        declare(name, h, h || is_scalar);
        auto sc = scalar.emplace(name, is_scalar);
        if (!sc.second)
            sc.first->second = sc.first->second && is_scalar;
    }

    // True for a name that is declared in the body as a number: "x << 1" does not change it.
    bool is_scalar_name(const Root& r) const
    {
        if (!r.name)
            return false;
        auto it = scalar.find(*r.name);
        return it != scalar.end() && it->second;
    }

    bool is_handle_name(const std::string& name) const
    {
        auto it = handle.find(name);
        return it != handle.end() && it->second;
    }

    // A use that goes through the path of a name. A handle stays unchanged by a use that goes to
    // its target. Any other value is changed.
    void write_through(const Root& r)
    {
        if (!r.name)
            return;
        if (r.exact || !is_handle_name(*r.name))
            written.insert(*r.name);
    }

    // The script function that a call names, when the callee is not a local.
    const Method* script_callee(const CallExpr& c) const
    {
        if (!ctx.script_fn || !c.callee)
            return nullptr;
        if (auto* n = std::get_if<NameExpr>(&c.callee->v))
            if (n->parts.size() == 1 && n->template_args.empty() && !declared.count(n->parts[0]))
                return ctx.script_fn(n->parts[0]);
        if (auto* m = std::get_if<MemberExpr>(&c.callee->v))
            if (m->obj && std::holds_alternative<ThisExpr>(m->obj->v))
                return ctx.script_fn(m->name);
        return nullptr;
    }

    void collect_declarations(const std::vector<Param>& params, const BlockStmt& body)
    {
        for (const Param& p : params)
            declare_typed(p.name, p.script_type.empty() ? p.type : p.script_type, p.value, nullptr);
        auto fe = [&](const Expr& e)
        {
            if (auto* l = std::get_if<LambdaExpr>(&e.v))
                for (const Param& p : l->params)
                    declare_typed(p.name, p.script_type.empty() ? p.type : p.script_type, p.value, nullptr);
            else if (auto* t = std::get_if<TypeTestExpr>(&e.v))
            {
                if (!t->bind.empty())
                    declare(t->bind, false, false);
            }
        };
        auto fs = [&](const Stmt& s)
        {
            if (auto* d = std::get_if<VarDeclStmt>(&s.v))
            {
                for (const std::string& n : d->names)
                    declare(n, false, false);
                if (d->names.empty())
                {
                    if (d->is_ref)
                        declare(d->name, false, false);
                    else
                        declare_typed(d->name, d->type, d->value, d->init.get());
                }
            }
            else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
            {
                for (const std::string& n : fo->names)
                    declare(n, false, false);
                if (fo->names.empty())
                    declare(fo->name, false, false);
            }
            else if (auto* tr = std::get_if<TryStmt>(&s.v))
                for (const CatchClause& c : tr->catches)
                    declare(c.name, false, false);
        };
        walk::make_walker(fe, fs).block(body);
    }

    void collect_writes(const BlockStmt& body)
    {
        auto fe = [&](const Expr& e)
        {
            if (auto* b = std::get_if<BinaryExpr>(&e.v))
            {
                if (is_assign_op(b->op) && b->lhs)
                    write_through(root_of(*b->lhs));
                // "s << x" and "s >> x" can be stream operators: they change the left operand, and ">>" the
                // right operand. A number does not change.
                else if ((b->op == "<<" || b->op == ">>") && b->lhs && b->rhs)
                {
                    const Root l = root_of(*b->lhs);
                    if (is_scalar_name(l) || is_literal_scalar(*b->lhs))
                        return; // a shift of numbers
                    write_through(l);
                    if (b->op == ">>")
                        write_through(root_of(*b->rhs));
                }
            }
            else if (auto* u = std::get_if<UnaryExpr>(&e.v))
            {
                if (u->operand && (u->op == "++" || u->op == "--" || (u->op == "&" && !u->postfix)))
                    write_through(root_of(*u->operand));
                // "await t": the members of an awaiter are not const.
                else if (u->operand && u->op == "await" && !u->postfix)
                    write_through(root_of(*u->operand));
            }
            else if (auto* ix = std::get_if<IndexExpr>(&e.v))
            {
                // "x[i]" can call a non-const operator[] (std::map inserts a missing key).
                if (ix->obj)
                {
                    Root r = root_of(*ix->obj);
                    r.exact = false;
                    write_through(r);
                }
            }
            else if (auto* c = std::get_if<CallExpr>(&e.v))
            {
                if (!c->callee)
                    return;
                // "x.M()": the member function can change the object.
                if (auto* n = std::get_if<NameExpr>(&c->callee->v))
                {
                    if (n->parts.size() >= 2)
                        write_through({&n->parts[0], false});
                    else if (n->parts.size() == 1)
                        written.insert(n->parts[0]); // a callable local: the call can change it
                }
                else if (auto* m = std::get_if<MemberExpr>(&c->callee->v))
                {
                    if (m->obj)
                        write_through({root_of(*m->obj).name, false});
                }
                const Method* callee = script_callee(*c);
                for (size_t i = 0; i < c->args.size(); ++i)
                {
                    if (!c->args[i])
                        continue;
                    const Root r = root_of(*c->args[i]);
                    if (!r.name)
                        continue;
                    if (callee && i < callee->params.size())
                    {
                        const Param& p = callee->params[i];
                        // A "T[]" parameter is a view that can write the source.
                        const bool by_ref = p.mode == ParamMode::Ref || p.mode == ParamMode::Out || is_ref_type(p.type)
                            || is_mutable_view(p.script_type.empty() ? p.type : p.script_type);
                        if (!by_ref)
                            continue;
                    }
                    write_through(r);
                }
            }
            else if (auto* nw = std::get_if<NewExpr>(&e.v))
            {
                // The constructor is not known here: an argument can go to a reference.
                for (const ExprP& a : nw->args)
                    if (a)
                        write_through(root_of(*a));
            }
        };
        auto fs = [&](const Stmt& s)
        {
            if (auto* d = std::get_if<VarDeclStmt>(&s.v))
            {
                // "ref r = x;" and "T& r = x;" bind a writable alias.
                // A local "T[] s = x;" is a view that can write "x".
                if (d->init && (d->is_ref || is_ref_type(d->type) || is_mutable_view(d->type)))
                    write_through(root_of(*d->init));
            }
            else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
            {
                if (fo->is_ref && fo->range)
                    write_through(root_of(*fo->range));
            }
            else if (auto* r = std::get_if<ReturnStmt>(&s.v))
            {
                if (r->expr)
                {
                    const Root root = root_of(*r->expr);
                    if (root.name && root.exact)
                    {
                        auto it = copy_ok.find(*root.name);
                        if (it == copy_ok.end() || !it->second)
                            written.insert(*root.name);
                    }
                }
            }
        };
        walk::make_walker(fe, fs).block(body);
    }

    void run(const std::vector<Param>& params, const BlockStmt& body)
    {
        collect_declarations(params, body);
        collect_writes(body);
    }

    // Sets "is_const" on the locals that the body never writes.
    void mark_locals(const BlockStmt& body)
    {
        // The walker gives const nodes. The pass owns the class, so it can set the flag.
        auto fe = [](const Expr&) {};
        auto fs = [&](const Stmt& s)
        {
            if (auto* d = std::get_if<VarDeclStmt>(&s.v))
            {
                if (d->names.empty() && d->init && !d->is_ref && !is_ref_type(d->type) && !d->weak
                    && !written.count(d->name))
                    const_cast<VarDeclStmt*>(d)->is_const = true;
            }
            else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
            {
                // A "ref" target that the body never writes binds a const reference.
                if (fo->names.empty() && fo->is_ref && !written.count(fo->name))
                    const_cast<ForOfStmt*>(fo)->is_const = true;
            }
        };
        walk::make_walker(fe, fs).block(body);
    }
};

bool params_default_ok(const Method& m)
{
    return !m.is_virtual && !m.is_override && !m.overrides_virtual && !m.is_abstract && !m.is_async && !m.raw;
}

} // namespace

DirectWrites direct_writes(const ast::Method& m)
{
    static const ConstContext no_context;
    BodyInfo info{no_context, {}, {}, {}, {}};
    info.collect_declarations(m.params, m.body);
    DirectWrites out;
    auto write = [&](const ExprP& target)
    {
        if (!target)
            return;
        const Root r = root_of(*target, true);
        if (r.name && !info.declared.count(*r.name))
        {
            auto it = out.roots.emplace(*r.name, r.exact);
            it.first->second = it.first->second || r.exact;
        }
    };
    auto fe = [&](const Expr& e)
    {
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (is_assign_op(b->op))
                write(b->lhs);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            if (u->op == "++" || u->op == "--")
                write(u->operand);
        }
        else if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            if (!c->callee)
                return;
            if (auto* n = std::get_if<NameExpr>(&c->callee->v))
            {
                if (n->parts.size() == 1 && !info.declared.count(n->parts[0]))
                    out.calls.insert(n->parts[0]);
            }
            else if (auto* mem = std::get_if<MemberExpr>(&c->callee->v))
            {
                if (mem->obj && std::holds_alternative<ThisExpr>(mem->obj->v))
                    out.calls.insert(mem->name);
            }
        }
    };
    auto fs = [](const Stmt&) {};
    walk::make_walker(fe, fs).block(m.body);
    return out;
}

void infer_const(ClassDef& cd, const ConstContext& ctx)
{
    for (Method& m : cd.methods)
    {
        if (m.raw)
            continue;
        BodyInfo info{ctx, {}, {}, {}, {}};
        info.run(m.params, m.body);
        info.mark_locals(m.body);
        if (ctx.params_ok ? !ctx.params_ok(m) : !params_default_ok(m))
            continue;
        for (Param& p : m.params)
        {
            const bool plain = p.mode == ParamMode::Plain || p.mode == ParamMode::ByVal;
            if (plain && !p.weak && !p.value && !is_ref_type(p.type) && !info.written.count(p.name))
                p.body_const = true;
        }
    }
    for (Ctor& c : cd.ctors)
    {
        if (c.raw)
            continue;
        BodyInfo info{ctx, {}, {}, {}, {}};
        info.run(c.params, c.body);
        info.mark_locals(c.body);
    }
    for (Method& m : cd.dtors)
    {
        if (m.raw)
            continue;
        BodyInfo info{ctx, {}, {}, {}, {}};
        info.run(m.params, m.body);
        info.mark_locals(m.body);
    }
}

} // namespace hotc
