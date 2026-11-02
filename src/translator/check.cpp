// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The checks of the script AST. See check.h.
#include "check.h"

#include "builtin_types.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <utility>
#include <variant>
#include <vector>

namespace hotc
{

namespace
{

using namespace ast;

// ------------------------------------------------------------------ walker
// Visits the nodes in the order of the script text, parent first. The walk goes into the
// bodies of lambdas. "on_expr" and "on_stmt" are called for each node.
template <class FE, class FS> struct Walker
{
    FE& on_expr;
    FS& on_stmt;
    std::function<void()> lambda_end; // optional: called after the body of each lambda

    void expr(const ExprP& e)
    {
        if (e)
            expr(*e);
    }

    void expr(const Expr& e)
    {
        on_expr(e);
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            expr(m->obj);
        }
        else if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            expr(c->callee);
            for (const ExprP& a : c->args)
                expr(a);
        }
        else if (auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            expr(ix->obj);
            expr(ix->index);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            expr(u->operand);
        }
        else if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            expr(b->lhs);
            expr(b->rhs);
        }
        else if (auto* t = std::get_if<TernaryExpr>(&e.v))
        {
            expr(t->cond);
            expr(t->a);
            expr(t->b);
        }
        else if (auto* p = std::get_if<ParenExpr>(&e.v))
        {
            expr(p->inner);
        }
        else if (auto* n = std::get_if<NewExpr>(&e.v))
        {
            for (const ExprP& a : n->args)
                expr(a);
        }
        else if (auto* tt = std::get_if<TypeTestExpr>(&e.v))
        {
            expr(tt->obj);
        }
        else if (auto* ip = std::get_if<InterpExpr>(&e.v))
        {
            for (const ExprP& a : ip->args)
                expr(a);
        }
        else if (auto* co = std::get_if<CollectionExpr>(&e.v))
        {
            for (const ExprP& a : co->elems)
                expr(a);
            for (const ExprP& a : co->values)
                expr(a);
        }
        else if (auto* l = std::get_if<LambdaExpr>(&e.v))
        {
            block(l->body);
            if (lambda_end)
                lambda_end();
        }
    }

    void block(const BlockStmt& b)
    {
        for (const StmtP& s : b.stmts)
            stmt(s);
    }

    void stmt(const StmtP& s)
    {
        if (s)
            stmt(*s);
    }

    void stmt(const Stmt& s)
    {
        on_stmt(s);
        if (auto* b = std::get_if<BlockStmt>(&s.v))
        {
            block(*b);
        }
        else if (auto* es = std::get_if<ExprStmt>(&s.v))
        {
            expr(es->expr);
        }
        else if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            expr(d->init);
        }
        else if (auto* i = std::get_if<IfStmt>(&s.v))
        {
            expr(i->cond);
            stmt(i->then);
            stmt(i->otherwise);
        }
        else if (auto* w = std::get_if<WhileStmt>(&s.v))
        {
            expr(w->cond);
            stmt(w->body);
        }
        else if (auto* dw = std::get_if<DoWhileStmt>(&s.v))
        {
            stmt(dw->body);
            expr(dw->cond);
        }
        else if (auto* f = std::get_if<ForStmt>(&s.v))
        {
            for (const StmtP& x : f->init)
                stmt(x);
            expr(f->cond);
            for (const ExprP& x : f->step)
                expr(x);
            stmt(f->body);
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            expr(fo->range);
            stmt(fo->body);
        }
        else if (auto* r = std::get_if<ReturnStmt>(&s.v))
        {
            expr(r->expr);
        }
        else if (auto* sw = std::get_if<SwitchStmt>(&s.v))
        {
            expr(sw->subject);
            for (const SwitchCase& c : sw->cases)
            {
                for (const ExprP& l : c.labels)
                    expr(l);
                for (const StmtP& x : c.stmts)
                    stmt(x);
            }
        }
        else if (auto* t = std::get_if<ThrowStmt>(&s.v))
        {
            expr(t->expr);
        }
        else if (auto* tr = std::get_if<TryStmt>(&s.v))
        {
            block(tr->block);
            for (const CatchClause& c : tr->catches)
                block(c.block);
            if (tr->finally_block)
                block(*tr->finally_block);
        }
    }
};

template <class FE, class FS> Walker<FE, FS> make_walker(FE& fe, FS& fs)
{
    return Walker<FE, FS>{fe, fs, nullptr};
}

// ------------------------------------------------------------------ type text
std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

bool starts_with(const std::string& s, const char* p)
{
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

// A trailing "*" or "&" of the type. Gives the type without it in "base".
bool is_borrowed_form(const std::string& type, std::string& base)
{
    std::string t = trim(type);
    if (t.empty() || (t.back() != '*' && t.back() != '&'))
        return false;
    while (!t.empty() && (t.back() == '*' || t.back() == '&' || t.back() == ' '))
        t.pop_back();
    if (starts_with(t, "const "))
        t = trim(t.substr(6));
    base = t;
    return true;
}

// The arguments of the top level template argument list of a type, for example
// "std::vector<Foo*>" gives "Foo*". Gives an empty vector when there is none.
std::vector<std::string> template_args(const std::string& type)
{
    std::vector<std::string> out;
    const size_t open = type.find('<');
    const size_t close = type.rfind('>');
    if (open == std::string::npos || close == std::string::npos || close < open)
        return out;
    int depth = 0;
    std::string cur;
    for (size_t i = open + 1; i < close; ++i)
    {
        const char c = type[i];
        if (c == '<' || c == '(')
            ++depth;
        if (c == '>' || c == ')')
            --depth;
        if (c == ',' && depth == 0)
        {
            out.push_back(trim(cur));
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    if (!trim(cur).empty())
        out.push_back(trim(cur));
    return out;
}

// ------------------------------------------------------------------ the checker
struct Checker
{
    const ClassDef& cd;
    const TypeModel& types;
    const LineMap& map;
    Diagnostics& diag;
    bool strict; // true: a borrowed-storage finding is an error. False: a warning.
    bool is_cpp; // true: the class is from a ".cpp" script
    bool std_below_20; // true: the C++ standard of the build is older than C++20

    std::set<std::pair<uint32_t, std::string>> reported;
    bool in_async = false; // true while the body of an "async" method is in progress
    bool in_async_void = false; // the same, and the value type of the method is "void"
    std::set<std::string> fn_names, field_names;
    std::set<std::string> locals; // parameters and locals of the body in progress
    std::set<std::string> borrowed; // the locals that hold a borrowed pointer or a reference
    struct Scope
    {
        std::set<std::string> locals;
        std::map<std::string, std::string> local_types;
        std::set<std::string> borrowed, pointers;
    };
    std::vector<Scope> scopes; // the state at the start of each lambda that is open
    std::set<std::string> pointers; // the part of "borrowed" that is a pointer: a copy is borrowed too
    std::map<std::string, std::string> local_types; // the declared type of a local ("": not known)
    std::map<std::string, std::string> field_types; // the declared type of each field
    std::set<const Expr*> allowed_refs; // "&x" nodes in a place where they are valid
    std::set<const Expr*> callee_names; // names in the callee place of a call

    void error(uint32_t pos, const std::string& msg)
    {
        if (reported.insert({pos, msg}).second)
            diag.add(pos, map, msg);
    }

    // A finding of the borrowed-storage rule (7.10.4 and 7.10.6).
    void borrowed_finding(uint32_t pos, const std::string& msg)
    {
        if (strict)
            error(pos, msg);
        else if (reported.insert({pos, msg}).second)
            diag.warn(pos, map, msg);
    }

    // The rules of a field (or a module variable) that are not for a return value or a collection.
    // Gives true when the type is handled: valid, or reported as an error.
    // - "T*" of a native class is stored as hot::CheckedPtr<T>: valid.
    // - A reference cannot change its target and has no null: error.
    // - A pointer to a builtin or an enumeration has no owner: error.
    // "T**" and "T*&" are not handled here: they keep the borrowed message.
    bool field_pointer_rule(const std::string& t, const std::string& base, uint32_t pos)
    {
        std::string stem = trim(t);
        if (!stem.empty() && stem.back() == '&')
        {
            while (!stem.empty() && (stem.back() == '&' || stem.back() == ' '))
                stem.pop_back();
            if (!stem.empty() && stem.back() == '*')
                return false;
            error(pos,
                "a field cannot be a reference: a reference cannot change its target and has no null. "
                "Use 'T*' (a checked pointer) or a value");
            return true;
        }
        if (types.is_value_pointer(t))
        {
            error(pos,
                "a field cannot hold a pointer to the value type '" + script_type_text(base)
                    + "': nothing owns it, so the pointer can dangle. "
                      "Use a value, or a pointer to the object that owns it");
            return true;
        }
        return !types.checked_ptr_storage(t).empty();
    }

    // ---- the storage rule (7.10.4 and 7.10.6)
    // "what" names the place: "a field", "a return value", "a collection element".
    void check_storage(const std::string& type, uint32_t pos, const std::string& what)
    {
        const std::string& t = type;
        std::string base;
        if (is_borrowed_form(t, base))
        {
            if (base == "char")
                return; // "const char*": the text of a string literal
            if (script_pointer(t, pos))
                return;
            const TypeRule rule = types.lookup(base);
            // An intrusive stub: "T*" is the handle. A "T&" or a "T**" is still borrowed.
            if (rule.kind == Kind::Handle && !rule.intrusive_add.empty() && !types.is_script_class(base)
                && is_single_pointer(t))
                return;
            if (what == "a field" && field_pointer_rule(t, base, pos))
                return;
            const std::string name = script_type_text(base);
            borrowed_finding(pos,
                what + " cannot hold a borrowed pointer or reference to '" + name
                    + "'. Declare a stub with 'handle' or 'intrusive' for '" + name + "', or use a value");
            return;
        }
        for (const std::string& arg : template_args(t))
            check_storage(arg, pos, "a collection element");
    }

    // A reload copies each field into the new state block (design 4.7). A field type must be
    // copyable. "std::unique_ptr" is the move-only type that a script can name.
    void check_copyable(const std::string& type, uint32_t pos)
    {
        std::string t = trim(type);
        if (t.compare(0, 5, "const") == 0 && t.size() > 5 && t[5] == ' ')
            t = trim(t.substr(6));
        for (const char* q : {"std.unique_ptr", "std::unique_ptr", "\x01std::unique_ptr"})
        {
            const size_t n = std::string(q).size();
            if (t.compare(0, n, q) == 0 && (t.size() == n || t[n] == '<' || t[n] == ' '))
            {
                error(pos, "field of move-only type '" + t + "' is not supported: a reload copies each field. "
                               "Use a handle");
                return;
            }
        }
        for (const std::string& arg : template_args(t))
            check_copyable(arg, pos);
    }

    // True when the type is "T*" or "const T*" with one star and no reference.
    static bool is_single_pointer(const std::string& type)
    {
        const std::string t = trim(type);
        if (t.empty() || t.back() != '*')
            return false;
        const std::string rest = trim(t.substr(0, t.size() - 1));
        return rest.empty() || (rest.back() != '*' && rest.back() != '&');
    }


    // A single "T*" of a script class is always an error: the class is already a handle.
    // Gives true when the type is such a pointer. A ".cpp" script has no handles: there the
    // pointer is the valid form, and it is not a borrowed pointer.
    bool script_pointer(const std::string& type, uint32_t pos)
    {
        std::string base;
        if (!is_borrowed_form(type, base) || !is_single_pointer(type) || !types.is_script_class(base))
            return false;
        if (is_cpp)
            return true;
        error(pos, "a pointer to the script class '" + base + "' is not allowed: use '" + base + "' (a handle)");
        return true;
    }

    // True when a local of this declared type holds a borrowed pointer or a reference.
    static bool is_borrowed_type(const std::string& type)
    {
        std::string base;
        return is_borrowed_form(type, base) && base != "char";
    }

    // True when libclang gives a "T*" of a type that is not a handle for a "var" local (S20b).
    // A handle type is a full owner, for example an intrusive "Actor*".
    bool is_borrowed_native(const std::string& type) const
    {
        std::string base;
        if (!is_borrowed_form(type, base) || base == "char")
            return false;
        return types.lookup(base).kind != Kind::Handle;
    }

    // ---- one body
    void body(const Method& m)
    {
        in_async = m.is_async && !is_cpp;
        in_async_void = in_async && (m.ret.empty() || m.ret == "void");
        body(m.params, m.body);
        in_async = in_async_void = false;
    }

    // The rules of an "async" method (a ".hot" script only). The script writes the value type:
    // the method returns "hot::Task<T>" in the C++ code.
    void check_async(const Method& m)
    {
        if (!m.is_async || is_cpp)
            return;
        if (cd.is_value)
            error(m.pos, "'async' is only valid on a class method");
        if (std_below_20)
            error(m.pos, "'async' needs C++20: set CMAKE_CXX_STANDARD 20 before hot_add_scripts (hotc: --std=c++20)");
        const std::string ret = trim(m.ret);
        if (ret.rfind("hot::Task<", 0) == 0 || ret.rfind("Task<", 0) == 0)
            error(m.pos, "'async' method '" + m.name + "' returns a task: write the value type, 'async' makes the task");
        for (const Param& p : m.params)
        {
            std::string base;
            if (is_borrowed_form(p.type, base))
                error(m.pos, "'async' method '" + m.name + "' cannot take parameter '" + p.name
                                 + "' by reference: the caller's object can end before the task continues");
        }
    }

    void body(const std::vector<Param>& params, const BlockStmt& block)
    {
        locals.clear();
        borrowed.clear();
        pointers.clear();
        local_types.clear();
        for (const Param& p : params)
        {
            record_local(p.name, p.type);
            if (is_borrowed_type(p.type))
                add_borrowed(p.name, p.type);
        }
        walk_block(block);
    }

    void walk_block(const BlockStmt& b)
    {
        auto fe = [&](const Expr& e) { on_expr(e); };
        auto fs = [&](const Stmt& s) { on_stmt(s); };
        auto w = make_walker(fe, fs);
        w.lambda_end = [&] { lambda_end(); };
        w.block(b);
    }

    // True when the expression is the prefix "&" (the address-of operator).
    static const UnaryExpr* as_address_of(const Expr& e)
    {
        const auto* u = std::get_if<UnaryExpr>(&e.v);
        return u && !u->postfix && u->op == "&" ? u : nullptr;
    }

    // True when the expression is a "&e" value.
    static bool is_borrow_form(const Expr& e) { return as_address_of(e) != nullptr; }

    // Allows each "&e" value that the expression passes on to a call or a local. The value can stand in
    // parentheses, in a branch of a ternary expression or of "??", and in a cast to a pointer
    // type. With "deref" it can also stand under a prefix "*", because "*(c ? &a : &b)" is a
    // reference for a call argument. Returns true when the expression has such a value.
    bool allow_borrow_forms(const Expr& e, bool deref)
    {
        if (is_borrow_form(e))
        {
            allowed_refs.insert(&e);
            return true;
        }
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner && allow_borrow_forms(*p->inner, deref);
        if (auto* t = std::get_if<TernaryExpr>(&e.v))
        {
            const bool a = t->a && allow_borrow_forms(*t->a, deref);
            const bool b = t->b && allow_borrow_forms(*t->b, deref);
            return a || b;
        }
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (b->op != "??")
                return false;
            const bool l = b->lhs && allow_borrow_forms(*b->lhs, deref);
            const bool r = b->rhs && allow_borrow_forms(*b->rhs, deref);
            return l || r;
        }
        if (auto* u = std::get_if<UnaryExpr>(&e.v); u && !u->postfix && u->operand)
        {
            if (u->op == "*")
                return deref && allow_borrow_forms(*u->operand, deref);
            if (u->op.size() >= 3 && u->op.front() == '(' && u->op.back() == ')')
            {
                const std::string ty = trim(u->op.substr(1, u->op.size() - 2));
                return !ty.empty() && ty.back() == '*' && allow_borrow_forms(*u->operand, deref);
            }
        }
        return false;
    }

    // Marks a local of a borrowed type. A pointer stays borrowed when a "var" copies it.
    void add_borrowed(const std::string& name, const std::string& type)
    {
        borrowed.insert(name);
        const std::string t = trim(type);
        if (!t.empty() && t.back() == '*')
            pointers.insert(name);
    }

    // True when the expression is a borrowed pointer local, also in parentheses, in a cast to a
    // pointer type or in a branch of a ternary expression.
    bool copies_borrowed_pointer(const Expr& e) const
    {
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner && copies_borrowed_pointer(*p->inner);
        if (auto* t = std::get_if<TernaryExpr>(&e.v))
            return (t->a && copies_borrowed_pointer(*t->a)) || (t->b && copies_borrowed_pointer(*t->b));
        // "p ?? q" gives p or q, so it copies a borrowed pointer when one side is borrowed.
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
            return b->op == "??"
                && ((b->lhs && copies_borrowed_pointer(*b->lhs)) || (b->rhs && copies_borrowed_pointer(*b->rhs)));
        if (auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            if (u->postfix || !u->operand || u->op.size() < 3 || u->op.front() != '(')
                return false;
            std::string ty = trim(u->op.substr(1, u->op.size() - 2));
            return !ty.empty() && ty.back() == '*' && copies_borrowed_pointer(*u->operand);
        }
        if (auto* n = std::get_if<NameExpr>(&e.v))
            return n->parts.size() == 1 && n->template_args.empty() && pointers.count(n->parts[0]) != 0;
        return false;
    }

    void record_local(const std::string& name, const std::string& type)
    {
        locals.insert(name);
        local_types[name] = type;
    }

    // The name of the handle that the operand of "&" is, else an empty string. A handle is a
    // field, a typed local or a parameter of a handle type, or "this". The type of a "var" local
    // is not known here, so the check skips it. The C++ compiler reports that case.
    std::string handle_operand(const Expr& e) const
    {
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner ? handle_operand(*p->inner) : std::string();
        if (std::holds_alternative<ThisExpr>(e.v))
            return "this";
        std::string name, type;
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (n->parts.size() != 1 || !n->template_args.empty())
                return {};
            name = n->parts[0];
        }
        else if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (!m->obj || !std::holds_alternative<ThisExpr>(m->obj->v))
                return {};
            auto f = field_types.find(m->name);
            if (f == field_types.end())
                return {};
            return types.is_handle(f->second) ? m->name : std::string();
        }
        else
        {
            return {};
        }
        auto l = local_types.find(name);
        if (l != local_types.end())
        {
            type = l->second;
        }
        else if (!locals.count(name))
        {
            auto f = field_types.find(name);
            if (f == field_types.end())
                return {};
            type = f->second;
        }
        return !type.empty() && types.is_handle(type) ? name : std::string();
    }

    void on_stmt(const Stmt& s)
    {
        if (auto* r = std::get_if<ReturnStmt>(&s.v))
            if (in_async_void && scopes.empty() && r->expr)
                error(r->pos, "an 'async void' method cannot return a value");
        if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            if (!d->names.empty())
            { // a structured binding: plain locals, "var&" borrows
                for (const std::string& n : d->names)
                {
                    record_local(n, std::string());
                    if (d->is_ref)
                        borrowed.insert(n);
                }
                return;
            }
            record_local(d->name, d->type);
            if (!d->type.empty())
                script_pointer(d->type, d->pos);
            if (d->is_ref)
            {
                borrowed.insert(d->name); // "var& r = x": a local reference
            }
            else if (d->init && allow_borrow_forms(*d->init, false))
            {
                add_borrowed(d->name, "*");
            }
            else if (!d->type.empty() && is_borrowed_type(d->type))
            {
                add_borrowed(d->name, d->type);
            }
            else if (d->type.empty() && is_borrowed_native(d->native_type))
            {
                add_borrowed(d->name, d->native_type); // "var w = a.GetWeapon()" with a native "Weapon*" (S20b)
            }
            else if (d->type.empty() && d->init && copies_borrowed_pointer(*d->init))
            {
                add_borrowed(d->name, "*"); // "var q = p": the copy of a borrowed pointer
            }
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            record_local(fo->name, fo->type);
            if (!fo->type.empty())
                script_pointer(fo->type, fo->pos);
            if (fo->is_ref)
                borrowed.insert(fo->name);
            for (const std::string& n : fo->names)
            {
                record_local(n, std::string());
                if (fo->is_ref)
                    borrowed.insert(n);
            }
        }
        else if (auto* tr = std::get_if<TryStmt>(&s.v))
        {
            for (const CatchClause& c : tr->catches)
                record_local(c.name, c.type);
        }
    }

    // A cast "(T&)e", "(T*)e" or "(T**)e" to a script class or a handle type is a C-style cast of
    // the handle storage in C++. It would reinterpret the handle.
    void check_cast(const UnaryExpr& u)
    {
        if (u.postfix || u.op.size() < 3 || u.op.front() != '(' || u.op.back() != ')')
            return;
        const std::string type = trim(u.op.substr(1, u.op.size() - 2));
        std::string base;
        if (!is_borrowed_form(type, base))
            return;
        if (types.is_script_class(base) || types.is_handle(base))
            error(u.pos,
                "a cast to '" + type + "' of the script class or handle '" + base
                    + "' is not allowed: use 'as' or static_cast<" + base + ">(...)");
    }

    void on_expr(const Expr& e)
    {
        if (auto* un = std::get_if<UnaryExpr>(&e.v))
        {
            check_cast(*un);
            if (!un->postfix && un->op == "await" && !is_cpp)
            {
                if (!scopes.empty())
                    error(un->pos, "'await' is only valid inside an 'async' method (a lambda cannot await yet)");
                else if (!in_async)
                    error(un->pos, "'await' is only valid inside an 'async' method");
            }
        }
        if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            callee_names.insert(c->callee.get());
            for (const ExprP& a : c->args)
                if (a)
                    allow_borrow_forms(*a, true);
        }
        else if (auto* n = std::get_if<NewExpr>(&e.v))
        {
            for (const ExprP& a : n->args)
                if (a)
                    allow_borrow_forms(*a, true);
        }
        else if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            // "q = p;": the local keeps a borrowed pointer from here on. The check never unmarks it.
            if (b->op == "=" && b->lhs && b->rhs && (copies_borrowed_pointer(*b->rhs) || is_borrow_form(*b->rhs)))
                if (auto* ln = std::get_if<NameExpr>(&b->lhs->v))
                    if (ln->parts.size() == 1 && ln->template_args.empty() && locals.count(ln->parts[0]))
                        add_borrowed(ln->parts[0], "*");
        }
        else if (const UnaryExpr* u = as_address_of(e))
        {
            if (u->operand)
            {
                const std::string h = handle_operand(*u->operand);
                if (!h.empty())
                    error(u->pos,
                        "'&' on the handle '" + h + "' is not allowed. Pass '" + h
                            + "' for a handle parameter or a native 'T*' or 'T&' parameter, or use '&*" + h + "' for a 'T*' and '*"
                            + h + "' for a 'T&'");
            }
            if (!allowed_refs.count(&e))
                borrowed_finding(u->pos,
                    "a '&' value cannot be stored in an assignment, a 'return' or a "
                    "field initializer. Use '&' only for a call argument or a local");
        }
        else if (auto* nm = std::get_if<NameExpr>(&e.v))
        {
            // 5.3: the script takes the address of a script function.
            if (nm->parts.size() == 1 && nm->template_args.empty() && fn_names.count(nm->parts[0])
                && !locals.count(nm->parts[0]) && !field_names.count(nm->parts[0]) && !callee_names.count(&e))
                error(nm->pos, "script function '" + nm->parts[0] +
                                   "' must be called. A script function has no address, because a reload "
                                   "replaces it");
        }
        else if (auto* l = std::get_if<LambdaExpr>(&e.v))
        {
            lambda(*l);
        }
    }

    // The lambda captures by value. A captured borrowed pointer or reference would outlive the call.
    void lambda(const LambdaExpr& l)
    {
        std::set<std::string> used, declared;
        for (const Param& p : l.params)
            declared.insert(p.name);
        auto fe = [&](const Expr& e)
        {
            if (auto* n = std::get_if<NameExpr>(&e.v))
                if (!n->parts.empty())
                    used.insert(n->parts[0]);
            if (auto* inner = std::get_if<LambdaExpr>(&e.v))
                for (const Param& p : inner->params)
                    declared.insert(p.name);
        };
        auto fs = [&](const Stmt& s)
        {
            if (auto* d = std::get_if<VarDeclStmt>(&s.v))
            {
                declared.insert(d->name);
                declared.insert(d->names.begin(), d->names.end());
            }
            if (auto* fo = std::get_if<ForOfStmt>(&s.v))
            {
                declared.insert(fo->name);
                declared.insert(fo->names.begin(), fo->names.end());
            }
        };
        make_walker(fe, fs).block(l.body);
        for (const std::string& n : used)
            if (!declared.count(n) && borrowed.count(n))
                borrowed_finding(l.pos, "the lambda captures '" + n + "', a borrowed pointer or reference. "
                             "A closure cannot keep it. Copy the value or use a handle");
        // The parameters and the locals of the body live only to the end of the lambda.
        scopes.push_back({locals, local_types, borrowed, pointers});
        for (const Param& p : l.params)
        {
            record_local(p.name, p.type);
            if (is_borrowed_type(p.type))
                add_borrowed(p.name, p.type);
        }
    }

    // The walker calls this after the body of a lambda.
    void lambda_end()
    {
        if (scopes.empty())
            return;
        Scope& sc = scopes.back();
        locals = std::move(sc.locals);
        local_types = std::move(sc.local_types);
        borrowed = std::move(sc.borrowed);
        pointers = std::move(sc.pointers);
        scopes.pop_back();
    }

    void run()
    {
        // A field and a method with one name collide in the state struct (a ".hot" script only).
        // A method that overrides a native virtual has no other check of this name.
        if (!is_cpp)
            for (const Field& f : cd.fields)
                for (const Method& m : cd.methods)
                    if (m.name == f.name)
                        diag.add(f.pos, map, "the field '" + f.name + "' has the name of a method of the class");
        for (const Method& m : cd.methods)
            if (!m.overrides_virtual)
                fn_names.insert(m.name);
        for (const Field& f : cd.fields)
        {
            field_names.insert(f.name);
            field_types[f.name] = f.type;
        }

        for (const Field& f : cd.fields)
        {
            std::string weak_base;
            if (f.weak && is_borrowed_form(f.type, weak_base) && is_single_pointer(f.type)
                && !types.checked_ptr_storage(f.type).empty())
                error(f.pos, "a pointer field is already a weak reference: remove '[weak]'");
            else
                check_storage(f.type, f.pos, "a field");
            // A reload copies the fields, and a task is move-only.
            if (!is_cpp && (mentions_task_type(f.type) || mentions_task_type(f.script_type)))
                error(f.pos, "a 'Task' field is not supported: hold the task in a local variable or in a host object");
            if (!is_cpp) // a ".cpp" field is the C++ of the user; the reload moves a field that does not copy
                check_copyable(f.type, f.pos);
            if (f.init)
            {
                locals.clear();
                borrowed.clear();
                pointers.clear();
                local_types.clear();
                auto fe = [&](const Expr& e) { on_expr(e); };
                auto fs = [&](const Stmt& s) { on_stmt(s); };
                auto w = make_walker(fe, fs);
                w.lambda_end = [&] { lambda_end(); };
                w.expr(*f.init);
            }
        }
        for (const Method& m : cd.methods)
        {
            check_async(m);
            for (const Param& p : m.params)
                script_pointer(p.type, m.pos);
            // An override keeps the native signature. The script cannot change its return type.
            if (!m.overrides_virtual && !m.ret.empty())
            {
                // The hidden field of an auto property reports the type, so skip the getter.
                const bool getter = m.name.rfind("get_", 0) == 0 && field_names.count("__p_" + m.name.substr(4));
                if (!getter)
                    check_storage(m.ret, m.pos, "a return value");
            }
            body(m);
        }
        for (const Ctor& c : cd.ctors)
        {
            for (const Param& p : c.params)
                script_pointer(p.type, c.pos);
            body(c.params, c.body);
        }
        for (const Method& d : cd.dtors)
            body(d);
    }
};

} // namespace

void check_class(const ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag,
    bool strict_borrowed, bool is_cpp, bool std_below_20)
{
    Checker{cd, types, map, diag, strict_borrowed, is_cpp, std_below_20, {}, false, {}, {}, {}, {}, {}, {}}.run();
}

bool cxx_std_below_20(const std::string& std_name)
{
    // "c++17", "gnu++14", "c++1z", "c++2b": the text after "++" says the year.
    const size_t p = std_name.find("++");
    if (p == std::string::npos)
        return false;
    const std::string v = std_name.substr(p + 2);
    for (const char* old : {"98", "03", "0x", "11", "14", "1y", "17", "1z"})
        if (v == old)
            return true;
    return false;
}

namespace
{

// The rules for one "[value] T" use. "what" names the construct, for example "field 'f'".
// "needs_ctor" is true for a declaration without an initializer: T must have a constructor
// with no parameters (a field default runs "new T()").
struct ValueUseChecker
{
    const std::vector<ClassDef>& classes;
    const TypeModel& types;
    const LineMap& map;
    Diagnostics& diag;

    const ClassDef* find(const std::string& name) const
    {
        for (const ClassDef& c : classes)
            if (c.name == name)
                return &c;
        return nullptr;
    }

    // True when the class can be built with no argument: it has no constructor, or one whose
    // parameters all have a default value.
    static bool has_default_ctor(const ClassDef& c)
    {
        if (c.ctors.empty())
            return true;
        for (const Ctor& k : c.ctors)
        {
            bool all_default = true;
            for (const Param& p : k.params)
                all_default = all_default && p.def;
            if (all_default)
                return true;
        }
        return false;
    }

    void use(const std::string& type, uint32_t pos, const std::string& what, bool needs_ctor, bool is_ref = false)
    {
        std::string t = trim(type);
        // A reference (also a const reference) is not a value: "[value]" holds its own copy.
        if (!t.empty() && t.back() == '&')
        {
            is_ref = true;
            t = trim(t.substr(0, t.size() - 1));
        }
        if (is_ref)
        {
            diag.add(pos, map,
                "[value] on a reference is not valid: a [value] holds its own copy. Remove the reference or the "
                "'[value]'");
            return;
        }
        // "const Pt" by value is the type Pt.
        if (starts_with(t, "const "))
            t = trim(t.substr(6));
        if (t.empty())
            return; // "[value] var x = e": the lowering decides
        if (types.is_script_class(t) && !types.is_script_value(t))
        {
            const std::string nb = types.script_native_base(t);
            if (!nb.empty())
            {
                diag.add(pos, map,
                    "[value] on " + what + " needs a script class with no native base, and '" + t
                        + "' has the native base '" + nb + "'. Remove '[value]'");
                return;
            }
            const ClassDef* c = find(t);
            if (needs_ctor && c && !has_default_ctor(*c))
                diag.add(pos, map,
                    "[value] on " + what + " has no initializer, so the class '" + t
                        + "' needs a constructor with no parameters. Add one, or write an initializer");
            return;
        }
        if (!types.is_script_value(t) && types.is_handle(t))
        {
            diag.add(pos, map,
                "[value] on " + what + " is not valid for the native type '" + t
                    + "'. The type rules decide the storage of a native type. Remove '[value]'");
            return;
        }
        diag.add(pos, map,
            "[value] on " + what + " has no effect: '" + script_type_text(t)
                + "' is a value type already. Remove '[value]'");
    }

    void locals(const BlockStmt& body)
    {
        auto fe = [](const Expr&) {};
        auto fs = [&](const Stmt& s)
        {
            const auto* d = std::get_if<VarDeclStmt>(&s.v);
            if (d && d->value)
                use(d->type, d->value_pos ? d->value_pos : d->pos, "local '" + d->name + "'", d->init == nullptr,
                    d->is_ref);
        };
        make_walker(fe, fs).block(body);
    }

    void run()
    {
        for (const ClassDef& cd : classes)
        {
            for (const Field& f : cd.fields)
            {
                if (!f.value)
                    continue;
                const uint32_t pos = f.value_pos ? f.value_pos : f.pos;
                if (cd.is_value)
                    diag.add(pos, map,
                        "[value] on field '" + f.name + "' of the struct '" + cd.name + "' is not supported yet");
                else
                    use(f.type, pos, "field '" + f.name + "'", !f.has_init);
            }
            auto params = [&](const std::vector<Param>& ps, uint32_t pos)
            {
                for (const Param& p : ps)
                    if (p.value)
                        use(p.type, pos, "parameter '" + p.name + "'", false);
            };
            for (const Method& m : cd.methods)
            {
                params(m.params, m.pos);
                if (m.ret_value)
                    use(m.ret, m.ret_value_pos ? m.ret_value_pos : m.pos, "the return type of '" + m.name + "'", false);
                locals(m.body);
            }
            for (const Ctor& c : cd.ctors)
            {
                params(c.params, c.pos);
                locals(c.body);
            }
            for (const Method& d : cd.dtors)
                locals(d.body);
        }
    }
};

} // namespace

void check_value_uses(const std::vector<ClassDef>& classes, const TypeModel& types, const LineMap& map, Diagnostics& diag)
{
    ValueUseChecker{classes, types, map, diag}.run();
}

void check_ctors(const ClassDef& cd, const LineMap& map, Diagnostics& diag)
{
    std::set<std::string> members; // the names that a base argument cannot use
    for (const Field& f : cd.fields)
        members.insert(f.name);
    for (const Method& m : cd.methods)
        members.insert(m.name);
    for (const Property& p : cd.props)
        members.insert(p.name);
    if (cd.dtors.size() > 1)
        diag.add(cd.dtors[1].pos, map, "class '" + cd.name + "' has more than one destructor");
    // Overloads (S20a): C++ picks one by the argument types. Two overloads need other types.
    std::set<std::vector<std::string>> sigs;
    for (const Ctor& c : cd.ctors)
    {
        std::vector<std::string> sig;
        // "int" and "int&" are not two overloads: a call with a variable is ambiguous. A handle
        // "Bar", "Bar&" and "const Bar&" all lower to "const std::shared_ptr<Bar>&" (a redefinition).
        for (const Param& p : c.params)
        {
            std::string t = trim(p.type);
            if (t.size() > 1 && t.back() == '&' && t[t.size() - 2] != '&')
                t = trim(t.substr(0, t.size() - 1));
            if (starts_with(t, "const "))
                t = trim(t.substr(6));
            sig.push_back(t);
        }
        if (!sigs.insert(sig).second)
            diag.add(c.pos, map, "class '" + cd.name + "' has two constructors with the same parameter types");
    }
    for (const Ctor& c : cd.ctors)
    {
        std::set<std::string> params;
        for (const Param& p : c.params)
        {
            if (!params.insert(p.name).second)
                diag.add(c.pos, map, "constructor parameter '" + p.name + "' is declared twice");
        }
        if (c.base_args.empty())
            continue;
        const std::string rule = "the arguments of 'base(...)' may use the constructor parameters only";
        std::set<std::pair<uint32_t, std::string>> seen;
        auto bad = [&](uint32_t pos, const std::string& what)
        {
            if (seen.insert({pos, what}).second)
                diag.add(pos, map, what + ": " + rule);
        };
        auto fe = [&](const Expr& e)
        {
            if (auto* t = std::get_if<ThisExpr>(&e.v))
                bad(t->pos, "'this' is not allowed");
            else if (auto* sp = std::get_if<BaseExpr>(&e.v))
                bad(sp->pos, "'base' is not allowed");
            else if (auto* l = std::get_if<LambdaExpr>(&e.v))
                bad(l->pos, "a lambda is not allowed");
            else if (auto* n = std::get_if<NameExpr>(&e.v))
            {
                if (!n->parts.empty() && !params.count(n->parts[0]) && members.count(n->parts[0]))
                    bad(n->pos, "'" + n->parts[0] + "' is a member of the class");
            }
        };
        auto fs = [](const Stmt&) {};
        auto w = make_walker(fe, fs);
        for (const ExprP& a : c.base_args)
            w.expr(a);
    }
}

void check_base_ctor_access(const ClassDef& cd, const ClassDef& parent, const LineMap& map, Diagnostics& diag)
{
    if (parent.ctors.empty())
        return; // a public default constructor
    // A private constructor of the parent is for the parent only. A protected one is for
    // the subclass. The candidates are the constructors with this number of arguments.
    auto check = [&](size_t n, uint32_t pos)
    {
        bool any = false, usable = false;
        for (const Ctor& c : parent.ctors)
        {
            size_t required = 0;
            for (const Param& p : c.params)
                if (!p.def)
                    ++required;
            if (n < required || n > c.params.size())
                continue;
            any = true;
            usable = usable || c.access != Access::Private;
        }
        if (any && !usable)
            diag.add(pos, map, "the constructor is private in '" + parent.name + "'");
    };
    if (cd.ctors.empty())
    {
        check(0, cd.pos);
        return;
    }
    for (const Ctor& c : cd.ctors)
    {
        if (c.has_base_call)
            check(c.raw ? c.raw_base_argc : c.base_args.size(), c.base_pos);
        else
            check(0, c.pos);
    }
}

namespace
{

// The name at the root of an access path: "p", "p.x" (NameExpr with parts), "(p).x[1]".
const std::string* root_name(const Expr& e)
{
    if (auto* n = std::get_if<NameExpr>(&e.v))
        return n->parts.empty() ? nullptr : &n->parts[0];
    if (auto* m = std::get_if<MemberExpr>(&e.v))
        return root_name(*m->obj);
    if (auto* ix = std::get_if<IndexExpr>(&e.v))
        return root_name(*ix->obj);
    if (auto* p = std::get_if<ParenExpr>(&e.v))
        return root_name(*p->inner);
    return nullptr;
}

// True when the expression is exactly the name (maybe in parentheses).
bool is_exactly(const Expr& e, const std::string& name)
{
    if (auto* n = std::get_if<NameExpr>(&e.v))
        return n->parts.size() == 1 && n->parts[0] == name;
    if (auto* p = std::get_if<ParenExpr>(&e.v))
        return is_exactly(*p->inner, name);
    return false;
}

bool is_assign_op(const std::string& op)
{
    return op == "=" || (op.size() >= 2 && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=");
}

} // namespace

namespace
{

// The field that an access path writes: "f", "f.x", "this.f", "this.f.x", "f[1]". A bare name
// that is a local is not a field. Gives the name or null.
const std::string* field_root(
    const Expr& e, const std::set<std::string>& locals, const std::map<std::string, std::string>& fields)
{
    if (auto* n = std::get_if<NameExpr>(&e.v))
    {
        if (n->parts.empty() || locals.count(n->parts[0]) || !fields.count(n->parts[0]))
            return nullptr;
        return &n->parts[0];
    }
    if (auto* m = std::get_if<MemberExpr>(&e.v))
    {
        if (std::holds_alternative<ThisExpr>(m->obj->v))
            return fields.count(m->name) ? &m->name : nullptr;
        return field_root(*m->obj, locals, fields);
    }
    if (auto* ix = std::get_if<IndexExpr>(&e.v))
        return field_root(*ix->obj, locals, fields);
    if (auto* p = std::get_if<ParenExpr>(&e.v))
        return field_root(*p->inner, locals, fields);
    return nullptr;
}

// The name of the script function that a call names: "G(...)" or "this.G(...)". Else empty.
std::string script_callee(const CallExpr& c, const std::set<std::string>& locals)
{
    if (auto* n = std::get_if<NameExpr>(&c.callee->v))
        if (n->parts.size() == 1 && n->template_args.empty() && !locals.count(n->parts[0]))
            return n->parts[0];
    if (auto* m = std::get_if<MemberExpr>(&c.callee->v))
        if (std::holds_alternative<ThisExpr>(m->obj->v))
            return m->name;
    return {};
}

// The names that a function declares: the parameters and the locals of the body.
std::set<std::string> declared_names(const std::vector<Param>& params, const BlockStmt& body)
{
    std::set<std::string> out;
    for (const Param& p : params)
        out.insert(p.name);
    auto fe = [&](const Expr& e)
    {
        if (auto* l = std::get_if<LambdaExpr>(&e.v))
            for (const Param& p : l->params)
                out.insert(p.name);
    };
    auto fs = [&](const Stmt& s)
    {
        if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            out.insert(d->name);
            out.insert(d->names.begin(), d->names.end());
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            out.insert(fo->name);
            out.insert(fo->names.begin(), fo->names.end());
        }
        else if (auto* tr = std::get_if<TryStmt>(&s.v))
            for (const CatchClause& c : tr->catches)
                out.insert(c.name);
    };
    make_walker(fe, fs).block(body);
    return out;
}

std::set<std::string> declared_names(const Method& m)
{
    return declared_names(m.params, m.body);
}

// True when a value of the type "param" can be a part of a field of the type "field": the same
// type, or the type of a template argument (for example an element of a std::vector).
bool can_alias(const std::string& param, const std::string& field)
{
    if (trim(param) == trim(field))
        return true;
    for (const std::string& a : template_args(field))
        if (can_alias(param, a))
            return true;
    return false;
}

struct ParamAnalysis
{
    const ParamContext& ctx;
    std::set<std::pair<const void*, std::string>> visiting; // stops a cycle of calls

    // Adds the fields that the function writes, also through the script functions that it
    // calls. "seen" visits each function one time.
    void fields_written(const Method& m, std::set<const Method*>& seen, std::set<std::string>& out)
    {
        if (!seen.insert(&m).second || ctx.fields.empty())
            return;
        const std::set<std::string> locals = declared_names(m);
        auto mark = [&](const ExprP& e)
        {
            if (!e)
                return;
            if (const std::string* f = field_root(*e, locals, ctx.fields))
                out.insert(*f);
        };
        auto fe = [&](const Expr& e)
        {
            if (auto* b = std::get_if<BinaryExpr>(&e.v))
            {
                if (is_assign_op(b->op))
                    mark(b->lhs);
            }
            else if (auto* u = std::get_if<UnaryExpr>(&e.v))
            {
                if (u->op == "++" || u->op == "--" || (u->op == "&" && !u->postfix))
                    mark(u->operand);
            }
            else if (auto* c = std::get_if<CallExpr>(&e.v))
            {
                // "f.M()" or "this.f.M()": a member function can change the field.
                if (auto* n = std::get_if<NameExpr>(&c->callee->v))
                {
                    if (n->parts.size() >= 2 && !locals.count(n->parts[0]) && ctx.fields.count(n->parts[0]))
                        out.insert(n->parts[0]);
                }
                else if (auto* mm = std::get_if<MemberExpr>(&c->callee->v))
                {
                    if (!std::holds_alternative<ThisExpr>(mm->obj->v))
                        mark(mm->obj);
                }
                const std::string g = script_callee(*c, locals);
                if (!g.empty() && ctx.script_fn)
                    if (const Method* gm = ctx.script_fn(g))
                        fields_written(*gm, seen, out);
            }
        };
        auto fs = [](const Stmt&) {};
        make_walker(fe, fs).block(m.body);
    }

    static void merge(ParamUse& use, const ParamUse& sub)
    {
        use.stored = use.stored || sub.stored;
        use.mutated = use.mutated || sub.mutated || sub.rebound;
    }

    // The parameter is the argument "i" of a call that is not a script function of the class
    // (S25). The call can go to a local lambda, to a method of a script handle or to a native
    // function. The analysis follows the parameter into the callee. When it cannot, the
    // parameter counts as stored: the body takes a copy.
    void call_arg(ParamUse& use, const CallExpr& c, size_t i, const std::map<std::string, const LambdaExpr*>& lambdas,
        const std::set<std::string>& declared)
    {
        if (auto* n = std::get_if<NameExpr>(&c.callee->v))
        {
            if (n->parts.size() == 1 && declared.count(n->parts[0]))
            {
                auto it = lambdas.find(n->parts[0]);
                if (it != lambdas.end() && i < it->second->params.size() && !is_ref_type(it->second->params[i].type))
                {
                    const LambdaExpr* l = it->second;
                    merge(use, run_fn(l, l->params, l->body, l->params[i].name, nullptr));
                    return;
                }
                if (it != lambdas.end() && i < it->second->params.size())
                { // a "T&" parameter
                    use.mutated = use.rebound = true;
                    return;
                }
                use.stored = true; // a callable value that the analysis cannot see
                return;
            }
        }
        const CallTarget* t = ctx.call_target ? ctx.call_target(c) : nullptr;
        if (t && t->script && ctx.for_class)
        {
            // A call on a script handle: the declared method gives the mode and the body.
            ParamContext other = ctx.for_class(*t->script);
            const Method* tm = other.script_fn ? other.script_fn(t->name) : nullptr;
            if (tm)
            {
                if (i >= tm->params.size())
                    return;
                if (is_ref_type(tm->params[i].script_type.empty() ? tm->params[i].type : tm->params[i].script_type))
                {
                    use.mutated = use.rebound = true; // a "T&" parameter: the callee can write it
                    return;
                }
                if (tm->is_virtual || tm->is_override)
                { // an override can do more: be careful
                    use.stored = true;
                    return;
                }
                ParamAnalysis sub{other, visiting};
                merge(use, sub.run(*tm, tm->params[i].name));
                return;
            }
        }
        if (t && !t->native.empty() && ctx.native_arg)
        {
            const char k = ctx.native_arg(t->native, i, c.args.size());
            if (k == 'c')
                return; // "const T&": no change and no store
            use.stored = true;
            if (k == 'm')
                use.mutated = true;
            return;
        }
        use.stored = true; // the callee is not known
    }

    // The use of a parameter of a script function. The lowering changes the body of a class
    // after its analysis, so the memo has the result of a class that is done (S25).
    ParamUse run(const Method& m, const std::string& name)
    {
        if (ctx.memo)
        {
            auto it = ctx.memo->find({&m, name});
            if (it != ctx.memo->end())
                return it->second;
        }
        return run_fn(&m, m.params, m.body, name, &m);
    }

    // The use of the parameter "name" in a function body. "m" is the method, or null for a
    // lambda. A lambda gets no alias check: the method that holds it has the check (S25).
    ParamUse run_fn(const void* key, const std::vector<Param>& params, const BlockStmt& body, const std::string& name,
        const Method* m)
    {
        ParamUse use;
        if (!visiting.insert({key, name}).second)
            return use; // a recursive call adds nothing new
        std::string ptype;
        for (const Param& p : params)
            if (p.name == name)
                ptype = p.script_type.empty() ? p.type : p.script_type;
        // The local lambdas by name: a call "f(p)" of one follows the parameter into the lambda.
        std::map<std::string, const LambdaExpr*> lambdas;
        {
            auto le = [](const Expr&) {};
            auto ls = [&](const Stmt& s)
            {
                if (auto* d = std::get_if<VarDeclStmt>(&s.v))
                    if (d->init)
                        if (auto* l = std::get_if<LambdaExpr>(&d->init->v))
                            lambdas[d->name] = l;
            };
            make_walker(le, ls).block(body);
        }
        const std::set<std::string> declared = declared_names(params, body);
        auto rooted = [&](const ExprP& e)
        {
            if (!e)
                return false;
            const std::string* r = root_name(*e);
            return r && *r == name;
        };
        // "p.M()" of a const native member function does not change "p".
        auto const_call = [&](const std::string& method)
        { return ctx.const_method && !ptype.empty() && ctx.const_method(ptype, method); };
        auto fe = [&](const Expr& e)
        {
            if (auto* b = std::get_if<BinaryExpr>(&e.v))
            {
                if (!is_assign_op(b->op))
                    return;
                if (rooted(b->lhs))
                    use.mutated = true;
                if (b->lhs && is_exactly(*b->lhs, name))
                    use.rebound = true;
                if (b->op == "=" && b->rhs && is_exactly(*b->rhs, name))
                    use.stored = true;
            }
            else if (auto* u = std::get_if<UnaryExpr>(&e.v))
            {
                if ((u->op == "++" || u->op == "--" || (u->op == "&" && !u->postfix)) && rooted(u->operand))
                {
                    use.mutated = true;
                    if (is_exactly(*u->operand, name))
                        use.rebound = true;
                }
            }
            else if (auto* c = std::get_if<CallExpr>(&e.v))
            {
                // "p.M()": a member function can change the object, unless it is const.
                if (auto* n = std::get_if<NameExpr>(&c->callee->v))
                {
                    if (n->parts.size() >= 2 && n->parts[0] == name
                        && !(n->parts.size() == 2 && const_call(n->parts[1])))
                        use.mutated = true;
                }
                else if (auto* mm = std::get_if<MemberExpr>(&c->callee->v))
                {
                    if (rooted(mm->obj) && !(is_exactly(*mm->obj, name) && const_call(mm->name)))
                        use.mutated = true;
                }
                // "G(p)" of a script function G: the use of the parameter of G counts (S20b).
                const std::string g = ctx.script_fn ? script_callee(*c, {name}) : std::string();
                const Method* gm = g.empty() ? nullptr : ctx.script_fn(g);
                for (size_t i = 0; i < c->args.size(); ++i)
                {
                    if (!c->args[i] || !is_exactly(*c->args[i], name))
                        continue;
                    if (gm)
                    {
                        if (i >= gm->params.size())
                            continue;
                        const Param& gp = gm->params[i];
                        if (is_ref_type(gp.script_type.empty() ? gp.type : gp.script_type))
                            use.mutated = use.rebound = true; // a "T&" parameter: the callee can write it
                        else
                            merge(use, run(*gm, gp.name));
                        continue;
                    }
                    call_arg(use, *c, i, lambdas, declared);
                }
            }
            else if (auto* nw = std::get_if<NewExpr>(&e.v))
            {
                for (const ExprP& a : nw->args)
                    if (a && is_exactly(*a, name))
                        use.stored = true;
            }
            else if (auto* l = std::get_if<LambdaExpr>(&e.v))
            {
                auto inner_e = [&](const Expr& x)
                {
                    if (auto* n = std::get_if<NameExpr>(&x.v))
                        if (!n->parts.empty() && n->parts[0] == name)
                            use.stored = true;
                };
                auto inner_s = [](const Stmt&) {};
                make_walker(inner_e, inner_s).block(l->body);
            }
        };
        auto fs = [&](const Stmt& s)
        {
            if (auto* r = std::get_if<ReturnStmt>(&s.v))
                if (r->expr && is_exactly(*r->expr, name))
                    use.stored = true;
            // "T& r = p;" and "var& r = p;" bind a writable alias to the parameter.
            if (auto* d = std::get_if<VarDeclStmt>(&s.v))
                if (d->init && (is_ref_type(d->type) || (d->is_ref && d->type.empty() && !d->is_const))
                    && rooted(d->init))
                {
                    use.mutated = true;
                    if (is_exactly(*d->init, name))
                        use.rebound = true;
                }
            // "foreach (var& e in p)" binds a writable alias to an element of the parameter.
            if (auto* fo = std::get_if<ForOfStmt>(&s.v))
                if (fo->is_ref && !fo->is_const && rooted(fo->range))
                    use.mutated = true;
        };
        make_walker(fe, fs).block(body);
        // A "const T&" can alias a field that the function writes (S20b): then take a copy.
        if (m && !ptype.empty() && !ctx.fields.empty())
        {
            std::set<const Method*> seen;
            std::set<std::string> written;
            fields_written(*m, seen, written);
            for (const std::string& f : written)
            {
                auto it = ctx.fields.find(f);
                if (it != ctx.fields.end() && can_alias(ptype, it->second))
                    use.aliased = true;
            }
        }
        visiting.erase({key, name});
        return use;
    }
};

} // namespace

ParamUse analyze_param_use(const Method& m, const std::string& name, const ParamContext& ctx)
{
    ParamAnalysis a{ctx, {}};
    const ParamUse use = a.run_fn(&m, m.params, m.body, name, &m);
    if (ctx.memo)
        (*ctx.memo)[{&m, name}] = use;
    return use;
}

// ------------------------------------------------------------------ native type queries
namespace
{

bool is_ident_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The type of an expression for the probe. "cpp" is a C++ type text that libclang can read.
// "script" is a script class (the type of "this", or a type that the script declares).
struct QType
{
    std::string cpp;
    const ClassDef* script = nullptr;
    bool known() const { return script || !cpp.empty(); }
};

struct QueryCollector
{
    Script& sc;
    std::map<std::string, const ClassDef*> classes;
    std::set<std::string> script_names; // the script classes and the script enums
    NativeQueries out;

    const ClassDef* cls = nullptr;
    bool allow_this = true;
    std::set<std::string> locals;
    std::map<std::string, QType> env;
    std::map<std::string, std::string> value_params; // name to the type: the parameters for "methods"
    std::set<std::string> param_names; // the by-value parameters of the body in progress

    // True when the type text names a script class or a script enum.
    bool mentions_script(const std::string& t) const
    {
        for (size_t i = 0; i < t.size();)
        {
            if (!is_ident_char(t[i]))
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < t.size() && is_ident_char(t[j]))
                ++j;
            const bool scoped = i >= 2 && t.compare(i - 2, 2, "::") == 0;
            if (!scoped && script_names.count(t.substr(i, j - i)))
                return true;
            i = j;
        }
        return false;
    }

    const ClassDef* parent_of(const ClassDef& c) const
    {
        auto it = classes.find(c.base);
        return c.base.empty() || it == classes.end() || it->second == &c ? nullptr : it->second;
    }

    // The native base of the root of a script class. Empty: none.
    std::string native_root(const ClassDef& c) const
    {
        const ClassDef* d = &c;
        for (int depth = 0; parent_of(*d) && depth < 64; ++depth)
            d = parent_of(*d);
        return d->base.empty() || classes.count(d->base) ? std::string() : d->base;
    }

    // The type of a declaration as the script writes it.
    QType of_text(const std::string& type) const
    {
        QType q;
        std::string t = trim(type), base = t;
        if (t.empty())
            return q;
        is_borrowed_form(t, base);
        auto it = classes.find(trim(base));
        if (it != classes.end())
        {
            q.script = it->second;
            return q;
        }
        if (!mentions_script(t))
            q.cpp = t;
        return q;
    }

    template <class FieldT> static const std::string& stype(const FieldT& f)
    {
        return f.script_type.empty() ? f.type : f.script_type;
    }

    // The member "name" of an object of the type "obj" (not a call).
    QType member(const QType& obj, const std::string& name) const
    {
        if (obj.script)
        {
            int depth = 0;
            for (const ClassDef* d = obj.script; d && depth < 64; d = parent_of(*d), ++depth)
            {
                for (const Field& f : d->fields)
                    if (f.name == name)
                        return of_text(stype(f));
                for (const Property& p : d->props)
                    if (p.name == name)
                        return of_text(stype(p));
            }
            const std::string nb = native_root(*obj.script);
            return nb.empty() ? QType{} : QType{"decltype(std::declval<" + nb + "&>()." + name + ")"};
        }
        if (obj.cpp.empty())
            return {};
        return QType{"decltype(std::declval<hotc_obj<" + obj.cpp + ">&>()." + name + ")"};
    }

    // The result of a call of the member function "name" of an object of the type "obj".
    QType method(const QType& obj, const std::string& name) const
    {
        if (obj.script)
        {
            int depth = 0;
            for (const ClassDef* d = obj.script; d && depth < 64; d = parent_of(*d), ++depth)
                for (const Method& m : d->methods)
                    if (m.name == name && !m.overrides_virtual)
                        return of_text(m.script_ret.empty() ? m.ret : m.script_ret);
            const std::string nb = native_root(*obj.script);
            return nb.empty() ? QType{} : QType{"decltype(hotc_ret_f(&" + nb + "::" + name + "))"};
        }
        if (obj.cpp.empty())
            return {};
        return QType{"decltype(hotc_ret_f(&hotc_obj<" + obj.cpp + ">::" + name + "))"};
    }

    // The type of a bare name: a local, a field of the class or a module variable.
    QType name_type(const std::string& n) const
    {
        if (locals.count(n))
        {
            auto it = env.find(n);
            return it == env.end() ? QType{} : it->second;
        }
        if (cls && allow_this)
        {
            QType self;
            self.script = cls;
            QType f = member(self, n);
            if (f.script || (!f.cpp.empty() && f.cpp.rfind("decltype", 0) != 0))
                return f;
        }
        for (const Field& v : sc.vars)
            if (v.name == n)
                return of_text(stype(v));
        return {};
    }

    // A scope chain "A.B.c": a static member or a name in a namespace. Gives "A::B::c" or empty.
    std::string scope_text(const std::vector<std::string>& parts, size_t n) const
    {
        if (n == 0 || script_names.count(parts[0]))
            return {};
        std::string r;
        for (size_t i = 0; i < n; ++i)
            r += (i ? "::" : "") + (i == 0 && n >= 2 && parts[0] == "std" ? std::string("\x01std") : parts[i]);
        return r;
    }

    bool is_head_value(const std::string& n) const { return locals.count(n) || name_type(n).known(); }

    QType type_of(const Expr& e) const
    {
        if (std::holds_alternative<ThisExpr>(e.v))
        {
            QType q;
            q.script = allow_this ? cls : nullptr;
            return q;
        }
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return type_of(*p->inner);
        if (auto* aw = std::get_if<UnaryExpr>(&e.v); aw && !aw->postfix && aw->op == "await" && aw->operand)
        { // "await" of a "Task<T>" gives T
            const std::string v = task_value_type(type_of(*aw->operand).cpp);
            return v.empty() ? QType{} : QType{v};
        }
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (n->parts.empty() || !n->template_args.empty())
                return {};
            if (n->parts.size() == 1)
                return name_type(n->parts[0]);
            if (is_head_value(n->parts[0]))
            {
                QType q = name_type(n->parts[0]);
                for (size_t i = 1; i < n->parts.size() && q.known(); ++i)
                    q = member(q, n->parts[i]);
                return q;
            }
            const std::string s = scope_text(n->parts, n->parts.size());
            return s.empty() ? QType{} : QType{"decltype(" + s + ")"};
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (m->name.empty() || std::holds_alternative<BaseExpr>(m->obj->v))
                return {};
            return member(type_of(*m->obj), m->name);
        }
        if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            if (auto* n = std::get_if<NameExpr>(&c->callee->v))
            {
                if (n->parts.empty() || !n->template_args.empty())
                    return {};
                if (n->parts.size() == 1)
                {
                    const std::string& f = n->parts[0];
                    if (locals.count(f))
                        return {};
                    if (cls)
                    {
                        QType self;
                        self.script = cls;
                        int depth = 0;
                        for (const ClassDef* d = cls; d && depth < 64; d = parent_of(*d), ++depth)
                            for (const Method& m : d->methods)
                                if (m.name == f && !m.overrides_virtual)
                                    return method(self, f);
                    }
                    return script_names.count(f) ? QType{} : QType{"decltype(hotc_ret_f(&" + f + "))"};
                }
                if (is_head_value(n->parts[0]))
                {
                    QType q = name_type(n->parts[0]);
                    for (size_t i = 1; i + 1 < n->parts.size() && q.known(); ++i)
                        q = member(q, n->parts[i]);
                    return q.known() ? method(q, n->parts.back()) : QType{};
                }
                const std::string s = scope_text(n->parts, n->parts.size());
                return s.empty() ? QType{} : QType{"decltype(hotc_ret_f(&" + s + "))"};
            }
            if (auto* m = std::get_if<MemberExpr>(&c->callee->v))
            {
                if (m->name.empty() || std::holds_alternative<BaseExpr>(m->obj->v))
                    return {};
                return method(type_of(*m->obj), m->name);
            }
        }
        return {};
    }

    void declare(const std::string& name, const QType& q)
    {
        locals.insert(name);
        if (q.known())
            env[name] = q;
        else
            env.erase(name);
    }

    // The member function "p.M()" on a value parameter "p".
    void note_method_call(const CallExpr& c)
    {
        std::string obj, fn;
        if (auto* n = std::get_if<NameExpr>(&c.callee->v))
        {
            if (n->parts.size() != 2)
                return;
            obj = n->parts[0];
            fn = n->parts[1];
        }
        else if (auto* m = std::get_if<MemberExpr>(&c.callee->v))
        {
            const std::string* r = simple_name_of(*m->obj);
            if (!r)
                return;
            obj = *r;
            fn = m->name;
        }
        auto it = value_params.find(obj);
        if (it != value_params.end() && !fn.empty())
            out.methods[it->second].insert(fn);
    }

    // The key of a native class for the lookup of its functions: no "const", no "*" or "&" and no
    // template arguments ("std::vector<int>" gives "std::vector"). Empty: not a plain name.
    static std::string type_key(const std::string& cpp)
    {
        std::string t = trim(cpp), r;
        if (t.rfind("const ", 0) == 0)
            t = trim(t.substr(6));
        int depth = 0;
        for (char ch : t)
        {
            if (ch == '<')
                ++depth;
            else if (ch == '>')
                --depth;
            else if (depth == 0 && ch != '*' && ch != '&' && ch != ' ')
                r += ch;
        }
        return r.find("decltype") != std::string::npos || r.find('(') != std::string::npos ? std::string() : r;
    }

    bool has_script_method(const ClassDef& c, const std::string& name) const
    {
        int depth = 0;
        for (const ClassDef* d = &c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Method& m : d->methods)
                if (m.name == name && !m.overrides_virtual)
                    return true;
        return false;
    }

    // What a call with a parameter as an argument calls (S25). Nothing is recorded for a call of
    // a script function of the class: the parameter analysis reads its body.
    void note_call_target(const CallExpr& c)
    {
        // The parameter analysis needs the calls that get a parameter. The call-site conversion
        // of a handle to a native parameter (task 33) needs the key of any call with arguments.
        bool has = false;
        for (const ExprP& a : c.args)
            has = has || a;
        if (!has)
            return;
        CallTarget t;
        auto from_receiver = [&](const QType& q, const std::string& fn)
        {
            if (q.script)
            {
                t.script = q.script;
                t.name = fn;
                const std::string nb = native_root(*q.script);
                if (!nb.empty())
                    t.native = type_key(nb) + "::" + fn;
            }
            else if (!q.cpp.empty())
            {
                const std::string k = type_key(q.cpp);
                if (!k.empty())
                    t.native = k + "::" + fn;
            }
        };
        if (auto* n = std::get_if<NameExpr>(&c.callee->v))
        {
            if (n->parts.empty() || !n->template_args.empty())
                return;
            if (n->parts.size() == 1)
            {
                const std::string& f = n->parts[0];
                if (locals.count(f) || script_names.count(f) || (cls && has_script_method(*cls, f)))
                    return;
                t.native = f;
            }
            else if (is_head_value(n->parts[0]))
            {
                QType q = name_type(n->parts[0]);
                for (size_t i = 1; i + 1 < n->parts.size() && q.known(); ++i)
                    q = member(q, n->parts[i]);
                if (q.known())
                    from_receiver(q, n->parts.back());
            }
            else
            {
                t.native = scope_text(n->parts, n->parts.size());
            }
        }
        else if (auto* m = std::get_if<MemberExpr>(&c.callee->v))
        {
            if (m->name.empty() || std::holds_alternative<BaseExpr>(m->obj->v))
                return;
            const QType q = type_of(*m->obj);
            if (std::holds_alternative<ThisExpr>(m->obj->v) && q.script && has_script_method(*q.script, m->name))
                return;
            if (q.known())
                from_receiver(q, m->name);
        }
        if (!t.script && t.native.empty())
            return;
        if (!t.native.empty())
            out.fn_keys.insert(t.native);
        out.calls[&c] = t;
    }

    static const std::string* simple_name_of(const Expr& e)
    {
        auto* n = std::get_if<NameExpr>(&e.v);
        return n && n->parts.size() == 1 ? &n->parts[0] : nullptr;
    }

    void body(const std::vector<Param>& params, const BlockStmt& block, const std::vector<Param>* extra = nullptr)
    {
        locals.clear();
        env.clear();
        value_params.clear();
        param_names.clear();
        auto add = [&](const Param& p)
        {
            const std::string& t = p.script_type.empty() ? p.type : p.script_type;
            if (!is_ref_type(t))
                param_names.insert(p.name);
            declare(p.name, of_text(t));
            const QType q = of_text(t);
            if (!is_ref_type(t) && !q.cpp.empty())
                value_params[p.name] = q.cpp;
        };
        for (const Param& p : params)
            add(p);
        if (extra)
            for (const Param& p : *extra)
                add(p);
        auto fe = [&](const Expr& e)
        {
            if (auto* l = std::get_if<LambdaExpr>(&e.v))
            {
                for (const Param& p : l->params)
                {
                    declare(p.name, of_text(p.type));
                    value_params.erase(p.name);
                }
            }
            else if (auto* c = std::get_if<CallExpr>(&e.v))
            {
                note_method_call(*c);
                note_call_target(*c);
            }
        };
        auto fs = [&](const Stmt& st)
        {
            if (auto* d = std::get_if<VarDeclStmt>(&st.v))
            {
                for (const std::string& n : d->names)
                {
                    value_params.erase(n);
                    declare(n, QType{});
                }
                if (!d->names.empty())
                    return;
                value_params.erase(d->name);
                if (!d->type.empty())
                {
                    declare(d->name, of_text(d->type));
                    return;
                }
                QType q;
                if (d->init)
                    q = type_of(*d->init);
                if (!q.cpp.empty())
                    out.vars.emplace_back(const_cast<VarDeclStmt*>(d), q.cpp);
                declare(d->name, q);
            }
            else if (auto* fo = std::get_if<ForOfStmt>(&st.v))
            {
                for (const std::string& n : fo->names)
                {
                    value_params.erase(n);
                    declare(n, QType{});
                }
                value_params.erase(fo->name);
                declare(fo->name, of_text(fo->type));
            }
            else if (auto* tr = std::get_if<TryStmt>(&st.v))
            {
                for (const CatchClause& c : tr->catches)
                    declare(c.name, QType{});
            }
        };
        make_walker(fe, fs).block(block);
    }

    void run()
    {
        for (const ClassDef& c : sc.classes)
        {
            classes[c.name] = &c;
            script_names.insert(c.name);
        }
        for (const EnumDef& en : sc.enums)
            script_names.insert(en.name);
        for (const ClassDef& c : sc.classes)
        {
            cls = &c;
            allow_this = true;
            for (const Method& m : c.methods)
                body(m.params, m.body);
            for (const Ctor& k : c.ctors)
                body(k.params, k.body);
            for (const Method& d : c.dtors)
                body(d.params, d.body);
            for (const Property& p : c.props)
            {
                if (p.get_body)
                    body({}, *p.get_body);
                if (p.set_body)
                {
                    Param v;
                    v.name = "value";
                    v.type = stype(p);
                    body({v}, *p.set_body);
                }
            }
        }
    }
};

} // namespace

NativeQueries collect_native_queries(Script& sc)
{
    QueryCollector q{sc, {}, {}, {}};
    q.run();
    return std::move(q.out);
}

bool uses_bare_this(const ClassDef& cd)
{
    std::set<const Expr*> member_objs;
    bool found = false;
    auto fe = [&](const Expr& e)
    {
        if (auto* m = std::get_if<MemberExpr>(&e.v))
            member_objs.insert(m->obj.get());
        if (std::holds_alternative<ThisExpr>(e.v) && !member_objs.count(&e))
            found = true;
    };
    auto fs = [](const Stmt&) {};
    auto w = make_walker(fe, fs);
    for (const Field& f : cd.fields)
        w.expr(f.init);
    for (const Method& m : cd.methods)
        w.block(m.body);
    for (const Method& d : cd.dtors)
        w.block(d.body);
    for (const Ctor& c : cd.ctors)
        w.block(c.body);
    for (const Property& p : cd.props)
    {
        if (p.get_body)
            w.block(*p.get_body);
        if (p.set_body)
            w.block(*p.set_body);
    }
    return found;
}

std::set<std::string> block_new_targets(const ClassDef& cd)
{
    std::set<std::string> out;
    auto fe = [&](const Expr& e)
    {
        if (auto* n = std::get_if<NewExpr>(&e.v))
            out.insert(n->type);
    };
    auto fs = [](const Stmt&) {};
    auto w = make_walker(fe, fs);
    for (const Field& f : cd.fields)
        w.expr(f.init);
    for (const Ctor& c : cd.ctors)
        for (const ExprP& a : c.base_args)
            w.expr(a);
    return out;
}

std::set<std::string> new_targets(const Script& sc)
{
    std::set<std::string> out;
    auto fe = [&](const Expr& e)
    {
        if (auto* n = std::get_if<NewExpr>(&e.v))
            out.insert(n->type);
    };
    auto fs = [](const Stmt&) {};
    auto w = make_walker(fe, fs);
    for (const ClassDef& cd : sc.classes)
    {
        for (const Field& f : cd.fields)
            w.expr(f.init);
        for (const Method& m : cd.methods)
            w.block(m.body);
        for (const Method& d : cd.dtors)
            w.block(d.body);
        for (const Ctor& c : cd.ctors)
        {
            w.block(c.body);
            for (const ExprP& a : c.base_args)
                w.expr(a);
        }
        for (const Property& p : cd.props)
        {
            w.expr(p.init);
            if (p.get_body)
                w.block(*p.get_body);
            if (p.set_body)
                w.block(*p.set_body);
        }
    }
    return out;
}

std::set<std::string> used_type_names(const ClassDef& cd)
{
    std::set<std::string> out;
    // Also the type behind a "*" or "&", and the template arguments: the check of a
    // borrowed pointer asks for the rule of the type behind it.
    std::function<void(std::string)> add = [&](std::string t)
    {
        if (t.empty())
            return;
        out.insert(t);
        std::string base;
        if (is_borrowed_form(t, base))
            add(base);
        else
            for (const std::string& a : template_args(t))
                add(a);
    };
    auto fe = [&](const Expr& e)
    {
        if (auto* n = std::get_if<NewExpr>(&e.v))
            add(n->type);
        if (auto* tt = std::get_if<TypeTestExpr>(&e.v); tt && (tt->is_as || !tt->bind.empty()))
            add(tt->type);
        if (auto* l = std::get_if<LambdaExpr>(&e.v))
            for (const Param& p : l->params)
                add(p.type);
    };
    auto fs = [&](const Stmt& s)
    {
        if (auto* d = std::get_if<VarDeclStmt>(&s.v))
            add(d->type);
        if (auto* fo = std::get_if<ForOfStmt>(&s.v))
            add(fo->type);
    };
    auto w = make_walker(fe, fs);
    for (const Field& f : cd.fields)
    {
        add(f.type);
        w.expr(f.init);
    }
    for (const Method& m : cd.methods)
    {
        add(m.ret);
        for (const Param& p : m.params)
            add(p.type);
        w.block(m.body);
    }
    for (const Method& d : cd.dtors)
        w.block(d.body);
    for (const Ctor& c : cd.ctors)
    {
        for (const Param& p : c.params)
            add(p.type);
        w.block(c.body);
    }
    for (const Property& p : cd.props)
    {
        add(p.type);
        w.expr(p.init);
        if (p.get_body)
            w.block(*p.get_body);
        if (p.set_body)
            w.block(*p.set_body);
    }
    return out;
}

std::set<std::string> used_type_names(const Script& sc)
{
    std::set<std::string> out;
    for (const ClassDef& cd : sc.classes)
    {
        std::set<std::string> one = used_type_names(cd);
        out.insert(one.begin(), one.end());
    }
    return out;
}

} // namespace hotc
