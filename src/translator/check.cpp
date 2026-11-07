// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The checks of the script AST. See check.h.
#include "check.h"

#include "ast_walk.h"
#include "builtin_types.h"
#include "const_infer.h"
#include "mutating_methods.h"
#include "operator_names.h"

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

using walk::make_walker;
using walk::Walker;

// ------------------------------------------------------------------ definite assignment
// A walk over the paths of a body for one name. "PathWalk" keeps one state: "assigned" says
// that the name has a value on the current path, and "alive" says that the path has not ended.
// A "return" or a "throw" ends the path. The merge of two paths keeps "assigned" only when each
// live path has it. A loop body can run zero times, so its assignments do not count after the loop.
// The null analyzer can reuse the walk: the rules for the paths are here, and the hooks give
// the events: a read of the name and a return (or the end of the body) with the state.
struct PathWalk
{
    struct State
    {
        bool assigned = false;
        bool alive = true;
    };

    const std::string& name;
    std::function<void(uint32_t pos)> on_read; // the name is read before it has a value
    std::function<void(uint32_t pos)> on_exit; // a path ends in a "return" with no value in the name
    State st;
    // The "break" states of the loops and the switches that are open: (some break seen, all assigned).
    std::vector<std::pair<bool, bool>> breaks;
    // One entry for each open loop: a "continue" was seen, and all "continue" states assigned.
    std::vector<std::pair<bool, bool>> continues;

    static bool is_name(const Expr& e, const std::string& n, bool whole)
    {
        auto* x = std::get_if<NameExpr>(&e.v);
        if (!x || x->parts.empty() || x->parts[0] != n)
            return false;
        return !whole || x->parts.size() == 1;
    }

    static State join(const State& a, const State& b)
    {
        if (!a.alive)
            return b;
        if (!b.alive)
            return a;
        return State{a.assigned && b.assigned, true};
    }

    // The nodes in a lambda body do not count: the lambda runs at another time.
    void reads(const ExprP& e)
    {
        if (e)
            expr(*e);
    }

    void expr(const Expr& e)
    {
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (!n->parts.empty() && n->parts[0] == name && !st.assigned)
                on_read(n->pos);
        }
        else if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            const bool plain = b->op == "=";
            const bool assign = plain
                || (b->op.size() >= 2 && b->op.back() == '=' && b->op != "==" && b->op != "!=" && b->op != "<="
                    && b->op != ">=");
            if (assign && b->lhs && is_name(*b->lhs, name, true))
            {
                if (!plain)
                    expr(*b->lhs); // "x += 1" reads x
                reads(b->rhs);
                st.assigned = true;
            }
            else if (b->op == "&&" || b->op == "||")
            {
                reads(b->lhs);
                const bool before = st.assigned; // the right side runs only on some paths
                reads(b->rhs);
                st.assigned = before;
            }
            else
            {
                reads(b->lhs);
                reads(b->rhs);
            }
        }
        else if (auto* t = std::get_if<TernaryExpr>(&e.v))
        {
            reads(t->cond);
            const bool before = st.assigned;
            reads(t->a);
            const bool a = st.assigned;
            st.assigned = before;
            reads(t->b);
            st.assigned = a && st.assigned;
        }
        else if (auto* c = std::get_if<CallExpr>(&e.v))
        {
            reads(c->callee);
            // The call site does not show the mode of the callee. A name that is a whole
            // argument can bind to an "out" or "ref" parameter, so the walk takes it as assigned.
            for (const ExprP& a : c->args)
            {
                if (a && is_name(*a, name, true))
                    st.assigned = true;
                else
                    reads(a);
            }
        }
        else if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            reads(m->obj);
        }
        else if (auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            reads(ix->obj);
            reads(ix->index);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            reads(u->operand);
        }
        else if (auto* p = std::get_if<ParenExpr>(&e.v))
        {
            reads(p->inner);
        }
        else if (auto* nw = std::get_if<NewExpr>(&e.v))
        {
            for (const ExprP& a : nw->args)
                reads(a);
        }
        else if (auto* tt = std::get_if<TypeTestExpr>(&e.v))
        {
            reads(tt->obj);
        }
        else if (auto* ip = std::get_if<InterpExpr>(&e.v))
        {
            for (const ExprP& a : ip->args)
                reads(a);
        }
        else if (auto* co = std::get_if<CollectionExpr>(&e.v))
        {
            for (const ExprP& a : co->elems)
                reads(a);
            for (const ExprP& a : co->values)
                reads(a);
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

    // Run "f" and restore the state after it. The result is the state at the end of "f".
    template <class F> State branch(F&& f)
    {
        const State saved = st;
        f();
        const State out = st;
        st = saved;
        return out;
    }

    // A loop body can run zero times: its assignments are lost.
    template <class F> void maybe(F&& f)
    {
        const State saved = st;
        breaks.emplace_back(false, true);
        continues.emplace_back(false, true);
        f();
        continues.pop_back();
        breaks.pop_back();
        st = saved;
    }

    void stmt(const Stmt& s)
    {
        if (!st.alive)
            return; // dead code after a return, a throw, a break or a continue
        if (auto* b = std::get_if<BlockStmt>(&s.v))
        {
            block(*b);
        }
        else if (auto* es = std::get_if<ExprStmt>(&s.v))
        {
            reads(es->expr);
        }
        else if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            reads(d->init);
        }
        else if (auto* i = std::get_if<IfStmt>(&s.v))
        {
            reads(i->cond);
            const State a = branch([&] { stmt(i->then); });
            const State b = branch([&] { stmt(i->otherwise); });
            st = join(a, b);
        }
        else if (auto* w = std::get_if<WhileStmt>(&s.v))
        {
            reads(w->cond);
            maybe([&] { stmt(w->body); });
        }
        else if (auto* dw = std::get_if<DoWhileStmt>(&s.v))
        {
            // The body runs at least one time. A "continue" jumps to the condition. The state at
            // the condition is the meet of the end of the body and the "continue" states. The
            // state after the loop is the meet of the condition state and the "break" states.
            breaks.emplace_back(false, true);
            continues.emplace_back(false, true);
            stmt(dw->body);
            const auto brk = breaks.back();
            const auto cont = continues.back();
            breaks.pop_back();
            continues.pop_back();
            State end = st;
            if (cont.first)
            {
                if (end.alive)
                    end.assigned = end.assigned && cont.second;
                else
                    end = State{cont.second, true};
            }
            st = end;
            if (st.alive)
                reads(dw->cond);
            if (!st.alive && brk.first)
                st = State{brk.second, true};
            else if (st.alive && brk.first)
                st.assigned = st.assigned && brk.second;
            else if (!st.alive)
                st = State{false, true}; // no path leaves the loop: keep the safe side
        }
        else if (auto* f = std::get_if<ForStmt>(&s.v))
        {
            for (const StmtP& x : f->init)
                stmt(x);
            reads(f->cond);
            maybe(
                [&]
                {
                    stmt(f->body);
                    for (const ExprP& x : f->step)
                        reads(x);
                });
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            reads(fo->range);
            maybe([&] { stmt(fo->body); });
        }
        else if (auto* r = std::get_if<ReturnStmt>(&s.v))
        {
            reads(r->expr);
            if (!st.assigned)
                on_exit(r->pos);
            st.alive = false;
        }
        else if (std::holds_alternative<BreakStmt>(s.v))
        {
            if (!breaks.empty())
            {
                breaks.back().first = true;
                breaks.back().second = breaks.back().second && st.assigned;
            }
            st.alive = false;
        }
        else if (std::holds_alternative<ContinueStmt>(s.v))
        {
            if (!continues.empty())
            {
                continues.back().first = true;
                continues.back().second = continues.back().second && st.assigned;
            }
            st.alive = false;
        }
        else if (auto* sw = std::get_if<SwitchStmt>(&s.v))
        {
            reads(sw->subject);
            const State start = st;
            bool has_default = false;
            State exit{false, false}; // no path yet
            breaks.emplace_back(false, true);
            for (const SwitchCase& c : sw->cases)
            {
                has_default = has_default || c.is_default;
                st = start; // a fall-through keeps at most what the start has
                for (const ExprP& l : c.labels)
                    reads(l);
                for (const StmtP& x : c.stmts)
                    stmt(x);
                if (&c == &sw->cases.back())
                    exit = join(exit, st);
            }
            const auto brk = breaks.back();
            breaks.pop_back();
            if (brk.first)
                exit = join(exit, State{brk.second, true});
            if (!has_default)
                exit = join(exit, start);
            st = exit;
        }
        else if (auto* t = std::get_if<ThrowStmt>(&s.v))
        {
            reads(t->expr);
            st.alive = false;
        }
        else if (auto* tr = std::get_if<TryStmt>(&s.v))
        {
            const State start = st;
            block(tr->block);
            State exit = st;
            for (const CatchClause& c : tr->catches)
            {
                st = start; // the try block can throw before any assignment
                block(c.block);
                exit = join(exit, st);
            }
            st = exit;
            if (tr->finally_block)
            {
                const bool live = st.alive;
                if (!live)
                    st = start;
                block(*tr->finally_block);
                st.alive = live && st.alive;
            }
        }
    }
};

// Report the "out" parameters that a body does not assign (design 7.10.1).
template <class Report>
void check_out_params(const std::vector<Param>& params, const BlockStmt& body, uint32_t end_pos, Report&& report)
{
    for (const Param& p : params)
    {
        if (p.mode != ParamMode::Out)
            continue;
        const std::string unassigned = "out parameter \"" + p.name + "\" is not assigned on every path";
        const std::string early = "out parameter \"" + p.name + "\" is read before it is assigned";
        PathWalk w{p.name, [&](uint32_t pos) { report(pos, early); }, [&](uint32_t pos) { report(pos, unassigned); }, {}, {}};
        w.block(body);
        if (w.st.alive && !w.st.assigned)
            report(end_pos, unassigned);
    }
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
// True when the type text has the word "ref" as a token: "ref T", "List<ref T>" or a tuple member.
bool has_ref_word(const std::string& type)
{
    auto word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    size_t i = 0;
    while (i < type.size())
    {
        if (!word_char(type[i]))
        {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < type.size() && word_char(type[j]))
            ++j;
        if (type.compare(i, j - i, "ref") == 0)
            return true;
        i = j;
    }
    return false;
}

// True when the type text names a view: "T[]", "string_view" or "span<". A view stored in a field or a
// container can outlive its source.
// A "function<...>" part is skipped: a view in its signature is a parameter or a result, not storage.
bool has_view_word(const std::string& type)
{
    std::string t = type;
    for (size_t p = t.find("function<"); p != std::string::npos; p = t.find("function<"))
    {
        size_t i = p + 9;
        for (int depth = 1; i < t.size() && depth > 0; ++i)
            depth += t[i] == '<' ? 1 : t[i] == '>' ? -1 : 0;
        t.erase(p, i - p);
    }
    return t.find("[]") != std::string::npos || t.find("string_view") != std::string::npos
        || t.find("span<") != std::string::npos;
}

constexpr const char* kRefStorageError =
    "'ref' cannot be stored in a field, a collection, a 'T?', a tuple or a handle. A 'ref' lives only in a local, "
    "a 'foreach' variable, a parameter or a 'ref' return. Use a value or '$T'";

// The place that a "ref" names (doc/type-design.md, "Lifetime of a ref"). "roots" are the names at the
// start of the place. A field of "this" has the root "this.name": it cannot collide with a local. "element"
// is true when the place can dangle after a change of its root: an element of a container, or a place
// behind a handle, a pointer, a "T?" or a "*". "any_field" is true for the result of a call of a "ref"
// method: it can be an element of any field of the object, so the change of the shape of any such field
// ends it.
struct Place
{
    std::set<std::string> roots;
    bool element = false;
    bool any_field = false;
    bool temporary = false;
    // Roots from an argument of a "ref" call that is not an element: a shape change of such a root does not
    // end the ref. A root in this set is also in "roots".
    std::set<std::string> plain_roots;

    // True when a shape change of the root "r" ends a ref with this place.
    bool shape_root(const std::string& r) const { return element && roots.count(r) && !plain_roots.count(r); }
};

// A live "ref" local or "foreach (ref ...)" variable.
struct RefInfo
{
    Place place;
    uint32_t pos = 0;
    bool loop = false;
    std::string type; // the declared type of the initializer, or empty when not known
};

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
    std::set<std::string> owning_locals; // the locals and by-value parameters that own their elements
    std::vector<std::set<std::string>> owning_stack; // owning_locals at the start of each open block
    // The view locals ("T[] s = v;", "std.string_view w = t;") and the root of the container that they
    // view. A view acts like a "ref" to an element of the root (doc/type-design.md, "Lifetime of a view").
    std::map<std::string, std::string> view_src;
    std::vector<std::map<std::string, std::string>> view_stack; // view_src at the start of each open block
    std::map<std::string, std::string> stale_views; // the views whose source changed shape: name -> cause
    // The stale marks of the statement in progress. They start at the end of the statement, because the
    // statement reads its own operands first ("v.push_back(s[0]);").
    std::map<std::string, std::string> pending_stale;
    std::vector<std::map<std::string, std::string>> stale_stack; // stale_views at the start of each open block
    bool last_stmt_exits = false; // the last statement that ended is a return, break, continue or throw
    std::string method_ret; // the declared return type of the method in progress
    std::map<std::string, std::string> field_types; // the declared type of each field
    std::set<const Expr*> allowed_refs; // "&x" nodes in a place where they are valid
    std::set<const Expr*> callee_names; // names in the callee place of a call
    std::map<std::string, RefInfo> refs; // the "ref" locals that are live in the body in progress
    // The "foreach (var x in c)" loops in progress. "c" is a local or a field of this. The body must not change
    // the shape of "c". A "foreach (ref ...)" loop uses "refs" instead.
    struct IterLoop
    {
        const Stmt* stmt;
        std::string root;
    };
    std::vector<IterLoop> iterating;
    std::vector<std::map<std::string, RefInfo>> refs_stack; // "refs" at the start of each open block
    std::set<std::string> ref_params; // the "ref" and "out" parameters of the body in progress
    // The live "ref" entries that the variable of a "foreach" hides. The end of the loop gives them back.
    std::map<const Stmt*, std::vector<std::pair<std::string, RefInfo>>> hidden_refs;
    // The refs of a declaration that start at the end of the declaration statement.
    std::map<const VarDeclStmt*, std::vector<std::pair<std::string, RefInfo>>> pending_refs;
    std::set<std::string> copy_params; // the plain, "in" and "[byval]" parameters of the body in progress
    bool method_ret_ref = false; // true while the body of a method with a "ref T" return is in progress
    bool method_ret_ptr = false; // true while the body of a method with a "T*" return of a value struct is in progress
    std::set<std::string> weak_fields; // the "[weak]" fields of the class
    std::set<std::string> weak_locals; // the "[weak]" parameters and locals of the body in progress
    const std::vector<ClassDef>* classes = nullptr; // the script classes of the unit, with the imported classes

    // True when the method returns "ref T". The checker runs before and after the lowering of the return type.
    static bool is_ref_ret(const Method& m) { return m.ret_ref || is_ref_return_text(m.ret); }

    // The own method (a method or a static) with a name and a "ref" return, or null.
    const Method* own_ref_method(const std::string& name) const
    {
        for (const std::vector<Method>* list : {&cd.methods, &cd.statics})
            for (const Method& m : *list)
                if (m.name == name && is_ref_ret(m))
                    return &m;
        return nullptr;
    }

    // True when the method is one of the statics of the class.
    bool is_own_static(const Method* m) const
    {
        for (const Method& s : cd.statics)
            if (&s == m)
                return true;
        return false;
    }

    // The root of a plain name: a field of this gets the tag "this.name". A local or a parameter keeps its name.
    std::string name_root(const std::string& n) const
    {
        return !locals.count(n) && field_names.count(n) ? "this." + n : n;
    }

    // The declared type of a local, a parameter or a field. Empty when not known (a "var" local).
    std::string name_type(const std::string& n) const
    {
        if (locals.count(n))
        {
            auto l = local_types.find(n);
            return l == local_types.end() ? std::string() : l->second;
        }
        auto f = field_types.find(n);
        return f == field_types.end() ? std::string() : f->second;
    }

    // The declared type of an object expression, or empty.
    std::string expr_type(const Expr& e) const
    {
        if (const auto* n = std::get_if<NameExpr>(&e.v))
            return n->parts.size() == 1 ? name_type(n->parts[0]) : std::string();
        if (const auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (m->obj && std::holds_alternative<ThisExpr>(m->obj->v))
            {
                auto f = field_types.find(m->name);
                return f == field_types.end() ? std::string() : f->second;
            }
            return std::string();
        }
        if (const auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner ? expr_type(*p->inner) : std::string();
        return std::string();
    }

    // True when a place behind a value of this type can dangle after the value is replaced: a handle, a
    // pointer, a "T?" or a box of a script class. A struct and a builtin value are not.
    bool is_replaceable_type(const std::string& type) const
    {
        std::string t = trim(type);
        if (t.rfind("const ", 0) == 0)
            t = trim(t.substr(6));
        if (t.empty())
            return false;
        if (t.back() == '?' || t.back() == '*' || t[0] == '$')
            return true;
        return types.is_handle(t) || (types.is_script_class(t) && !types.is_script_value(t));
    }

    // True when a change of a field of this type can move or remove the places inside it: any type that is
    // not a builtin scalar (a container, a string, an array, a struct or a replaceable value).
    bool can_reshape(const std::string& type) const
    {
        const std::string t = trim(type);
        if (t.empty())
            return false;
        // Any type that is not a builtin scalar can hold a container: a struct, for example.
        for (const BuiltinType& b : builtin_types())
            if (t == b.script || (b.alias && t == b.alias) || t == b.cpp)
                return false;
        return true;
    }

    // True when the type is a type of the prelude of std or eastl: "std.vector<int>", "eastl::string".
    // A lowered type starts with the mark of the namespace "std" (std_ns.h).
    static bool is_prelude_container(const std::string& type)
    {
        const std::string t = !type.empty() && type[0] == '\x01' ? type.substr(1) : type;
        for (const char* p : {"std.", "std::", "eastl.", "eastl::"})
            if (t.rfind(p, 0) == 0)
                return true;
        return false;
    }

    // The type text without "const ", "$" and the marks "&", "*" and "?".
    static std::string bare_type(const std::string& type)
    {
        std::string t = trim(type);
        if (t.rfind("const ", 0) == 0)
            t = trim(t.substr(6));
        while (!t.empty() && (t.back() == '&' || t.back() == '*' || t.back() == '?'))
            t = trim(t.substr(0, t.size() - 1));
        if (!t.empty() && t[0] == '$')
            t = trim(t.substr(1));
        return t;
    }

    // A method with a "ref" return of a script class. "is_static" is true for a static method.
    struct RefMethod
    {
        const Method* m = nullptr;
        bool is_static = false;
    };

    // The method "name" with a "ref" return of the script class of the type, else an empty result.
    RefMethod ref_method_of(const std::string& type, const std::string& name) const
    {
        const std::string t = bare_type(type);
        if (!classes || t.empty())
            return {};
        for (const ClassDef& c : *classes)
        {
            if (c.name != t && c.key_name() != t)
                continue;
            for (const Method& m : c.methods)
                if (m.name == name && is_ref_ret(m))
                    return {&m, false};
            for (const Method& m : c.statics)
                if (m.name == name && is_ref_ret(m))
                    return {&m, true};
            return {};
        }
        return {};
    }

    // The method or the static "name" of the script class of the type, else null.
    const Method* method_of(const std::string& type, const std::string& name) const
    {
        const std::string t = bare_type(type);
        if (!classes || t.empty())
            return nullptr;
        for (const ClassDef& c : *classes)
            if (c.name == t || c.key_name() == t)
            {
                for (const std::vector<Method>* list : {&c.methods, &c.statics})
                    for (const Method& m : *list)
                        if (m.name == name)
                            return &m;
                return nullptr;
            }
        return nullptr;
    }

    // True when the method returns "T*" where "T" is a value struct: a script struct or a native value
    // that is not a builtin scalar. The body returns "null" or "&place" (a ".hot" script only).
    bool is_ptr_ret(const Method& m) const
    {
        std::string base;
        if (is_cpp || is_ref_ret(m) || !is_single_pointer(m.ret) || !is_borrowed_form(m.ret, base))
            return false;
        base = bare_type(base);
        if (base.empty() || is_prelude_container(base) || !can_reshape(base))
            return false;
        if (types.is_script_class(base))
            return types.is_script_value(base);
        return types.lookup(base).kind == Kind::Value;
    }

    // The script method that a call calls: an own method or static, or a method of the class of the
    // receiver. Null when it is not known.
    const Method* called_method(const Expr& e) const
    {
        const auto* c = std::get_if<CallExpr>(&e.v);
        if (!c || !c->callee)
            return nullptr;
        auto own = [&](const std::string& name) -> const Method*
        {
            for (const std::vector<Method>* list : {&cd.methods, &cd.statics})
                for (const Method& m : *list)
                    if (m.name == name)
                        return &m;
            return nullptr;
        };
        if (const auto* cn = std::get_if<NameExpr>(&c->callee->v))
        {
            if (cn->parts.size() == 1)
                return locals.count(cn->parts[0]) ? nullptr : own(cn->parts[0]);
            if (cn->parts.size() != 2)
                return nullptr;
            const std::string& first = cn->parts[0];
            const bool is_var = locals.count(first) || field_names.count(first) || refs.count(first);
            return method_of(is_var ? name_type(first) : first, cn->parts[1]);
        }
        if (const auto* cm = std::get_if<MemberExpr>(&c->callee->v); cm && cm->obj)
            return std::holds_alternative<ThisExpr>(cm->obj->v) ? own(cm->name)
                                                                : method_of(expr_type(*cm->obj), cm->name);
        return nullptr;
    }

    // True when a call on a receiver of this type can give a reference into the receiver: a native class,
    // a handle or a pointer to one, or a type that is not known. A script class, a builtin scalar, a view,
    // an array and a type of the prelude give a temporary.
    bool is_native_receiver(const std::string& type) const
    {
        const std::string t = bare_type(type);
        if (t.empty())
            return true;
        if (t.find('[') != std::string::npos || is_prelude_container(t) || types.is_script_class(t))
            return false;
        return can_reshape(t); // false for a builtin scalar
    }

    // True when the chain of the expression starts at a "[weak]" handle. Each read of a weak handle locks it
    // and gives a temporary strong handle.
    bool starts_weak(const Expr& e) const
    {
        if (const auto* n = std::get_if<NameExpr>(&e.v))
            return !n->parts.empty() && (locals.count(n->parts[0]) ? weak_locals.count(n->parts[0]) != 0
                                                                   : weak_fields.count(n->parts[0]) != 0);
        if (const auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (!m->obj)
                return false;
            if (std::holds_alternative<ThisExpr>(m->obj->v))
                return weak_fields.count(m->name) != 0;
            return starts_weak(*m->obj);
        }
        if (const auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner && starts_weak(*p->inner);
        return false;
    }

    // The place of a plain name that starts a chain. "stepped" is true when the chain goes on with ".member".
    void name_place(const std::string& first, bool stepped, Place& out) const
    {
        out = Place();
        auto it = refs.find(first);
        if (it != refs.end())
        {
            out = it->second.place; // a "ref" local: the place that it stores
            // The owner behind the ref can be replaced when its type is not known or is replaceable.
            if (stepped && (it->second.type.empty() || is_replaceable_type(it->second.type)))
                out.element = true;
            return;
        }
        out.roots.insert(name_root(first)); // an unknown name is a module variable: a valid root
        if (stepped)
        {
            const std::string t = name_type(first);
            // A "var" local has no type text: treat it as a handle.
            if (is_replaceable_type(t) || (t.empty() && locals.count(first)))
                out.element = true;
        }
    }

    // R5 and D4: the place of the result of a call of the own "ref" method "m" is the shortest of the roots of
    // the object ("obj") and the roots of each argument at a "ref" or "out" parameter.
    bool call_place(const CallExpr& c, const Method& m, const Place& obj, Place& out) const
    {
        out = obj;
        out.element = true;
        out.any_field = true;
        for (size_t i = 0; i < c.args.size() && i < m.params.size(); ++i)
        {
            const ParamMode mode = m.params[i].mode;
            Place a;
            if (!c.args[i] || (mode != ParamMode::Ref && mode != ParamMode::Out) || !place_of(*c.args[i], a))
                continue;
            // The flags "element" and "any_field" are for the object only: an argument keeps its own flags.
            for (const std::string& r : a.roots)
            {
                if (out.roots.insert(r).second)
                {
                    if (!a.shape_root(r))
                        out.plain_roots.insert(r);
                }
                else if (a.shape_root(r))
                    out.plain_roots.erase(r);
            }
        }
        return true;
    }

    // Finds the place of an expression. Returns false for a temporary value: "out.temporary" is true.
    bool place_of(const Expr& e, Place& out) const
    {
        auto temp = [&]
        {
            out = Place();
            out.temporary = true;
            return false;
        };
        if (const auto* n = std::get_if<NameExpr>(&e.v))
        {
            if (n->parts.empty())
                return temp();
            name_place(n->parts[0], n->parts.size() > 1, out);
            return true;
        }
        if (std::holds_alternative<ThisExpr>(e.v) || std::holds_alternative<BaseExpr>(e.v))
        {
            out = Place();
            out.roots.insert("this");
            return true;
        }
        if (const auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (m->safe || !m->obj)
                return temp();
            if (std::holds_alternative<ThisExpr>(m->obj->v))
            { // a field of this: the field is the root
                out = Place();
                out.roots.insert("this." + m->name);
                return true;
            }
            if (!place_of(*m->obj, out))
                return temp();
            if (is_replaceable_type(expr_type(*m->obj)))
                out.element = true; // the owner can be replaced
            return true;
        }
        if (const auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            if (!ix->obj || !place_of(*ix->obj, out))
                return temp();
            out.element = true;
            return true;
        }
        if (const auto* pe = std::get_if<ParenExpr>(&e.v))
            return pe->inner && place_of(*pe->inner, out) ? true : temp();
        if (const auto* u = std::get_if<UnaryExpr>(&e.v))
        {
            if (u->postfix || u->op != "*" || !u->operand || !place_of(*u->operand, out))
                return temp();
            out.element = true; // the target of a handle or a pointer: the owner can be replaced
            return true;
        }
        if (const auto* t = std::get_if<TernaryExpr>(&e.v))
        {
            Place a, b;
            if (!t->a || !t->b || !place_of(*t->a, a) || !place_of(*t->b, b))
                return temp();
            out = a;
            out.roots.insert(b.roots.begin(), b.roots.end());
            out.element = a.element || b.element;
            out.any_field = a.any_field || b.any_field;
            for (const Place* side : {&a, &b})
                for (const std::string& r : side->plain_roots)
                    if (!a.shape_root(r) && !b.shape_root(r))
                        out.plain_roots.insert(r);
            return true;
        }
        if (const auto* c = std::get_if<CallExpr>(&e.v))
        {
            if (!c->callee)
                return temp();
            auto is_element_call = [](const std::string& name) { return name == "front" || name == "back" || name == "at"; };
            if (const auto* cn = std::get_if<NameExpr>(&c->callee->v))
            {
                // "xs.back()": the container is the first part of the name.
                if (cn->parts.size() >= 2 && is_element_call(cn->parts.back()))
                {
                    name_place(cn->parts[0], true, out);
                    out.element = true;
                    return true;
                }
                if (const Method* om = own_ref_method(cn->parts.back());
                    om && (cn->parts.size() > 1 || !locals.count(cn->parts[0])))
                {
                    Place obj; // a static method has no object: only the arguments are roots
                    if (!is_own_static(om))
                    {
                        if (cn->parts.size() == 1)
                            obj.roots.insert("this");
                        else
                            name_place(cn->parts[0], true, obj);
                    }
                    return call_place(*c, *om, obj, out);
                }
                // "o.F()" or "Class.F()" where "F" of another script class returns "ref": the roots of "o",
                // as an element, and the roots of each "ref" argument.
                if (cn->parts.size() == 2 && !starts_weak(*c->callee))
                {
                    const std::string& first = cn->parts[0];
                    const bool is_var = locals.count(first) || field_names.count(first) || refs.count(first);
                    if (is_var || types.is_script_class(first))
                    {
                        const RefMethod rm = ref_method_of(is_var ? name_type(first) : first, cn->parts[1]);
                        if (rm.m && (is_var || rm.is_static))
                        {
                            Place obj; // a static method has no object: only the arguments are roots
                            if (!rm.is_static)
                                name_place(first, true, obj);
                            return call_place(*c, *rm.m, obj, out);
                        }
                    }
                }
                // A native call "o.G()" on a native receiver "o": the place is an element of "o". The checker
                // does not know the real owner of the reference. A value return fails in the C++ compile.
                if (cn->parts.size() >= 2 && !starts_weak(*c->callee))
                {
                    const std::string& first = cn->parts[0];
                    const bool is_var = locals.count(first) || field_names.count(first) || refs.count(first);
                    const std::string t = cn->parts.size() == 2 && is_var ? name_type(first) : std::string();
                    if ((is_var || !types.is_script_class(first)) && (cn->parts.size() > 2 || is_native_receiver(t)))
                    {
                        name_place(first, true, out);
                        out.element = true;
                        return true;
                    }
                }
                return temp();
            }
            if (const auto* cm = std::get_if<MemberExpr>(&c->callee->v))
            {
                if (!cm->safe && cm->obj && is_element_call(cm->name) && place_of(*cm->obj, out))
                {
                    out.element = true;
                    return true;
                }
                if (const Method* om = cm->safe || !cm->obj ? nullptr : own_ref_method(cm->name))
                {
                    Place obj;
                    if (!is_own_static(om) && !place_of(*cm->obj, obj))
                        return temp();
                    return call_place(*c, *om, obj, out);
                }
                // A "ref" call on an object of another script class: see the NameExpr form above.
                if (!cm->safe && cm->obj && !starts_weak(*cm->obj))
                    if (const RefMethod rm = ref_method_of(expr_type(*cm->obj), cm->name); rm.m)
                    {
                        Place obj;
                        if (!rm.is_static && !place_of(*cm->obj, obj))
                            return temp();
                        return call_place(*c, *rm.m, obj, out);
                    }
                // A native call on a native receiver: see the NameExpr form above.
                if (!cm->safe && cm->obj && !std::holds_alternative<ThisExpr>(cm->obj->v)
                    && !std::holds_alternative<BaseExpr>(cm->obj->v) && !starts_weak(*cm->obj)
                    && is_native_receiver(expr_type(*cm->obj)) && place_of(*cm->obj, out))
                {
                    out.element = true;
                    return true;
                }
                return temp();
            }
        }
        return temp();
    }

    // A view local over a container ("T[] s = v;") acts like a "ref" to an element of the root of "v": a
    // later shape change of "v" makes the view stale, and a use of a stale view is an error.
    void track_view(const VarDeclStmt& d)
    {
        for (const std::string& n : d.names)
        {
            view_src.erase(n);
            stale_views.erase(n);
        }
        if (d.names.empty())
        {
            view_src.erase(d.name);
            stale_views.erase(d.name);
        }
        if (is_cpp || scopes.size() || d.is_ref || !d.names.empty() || !d.init || !is_view_type(d.type))
            return;
        if (const std::string r = root_name(*d.init); !r.empty() && (r.rfind("this.", 0) == 0 || owning_locals.count(r)))
            view_src[d.name] = r;
    }

    // R1 and the place of a "ref" local. A temporary is an error.
    void track_ref(const VarDeclStmt& d)
    {
        if (is_cpp || !d.init)
            return;
        Place pl;
        if (!place_of(*d.init, pl))
        {
            const std::string n = ref_names_text(d.name, d.names);
            error(d.pos, "'ref " + n + "' binds to a temporary value: it ends at the end of the statement. Use 'var " + n
                             + "' to copy the value");
            return;
        }
        // The ref starts after its own statement, so the call in its initializer does not hit it.
        RefInfo info{pl, d.pos, false, d.type.empty() ? expr_type(*d.init) : d.type};
        auto& pending = pending_refs[&d];
        if (!d.name.empty())
            pending.emplace_back(d.name, info);
        for (const std::string& n : d.names)
            pending.emplace_back(n, info);
    }

    // The loop variable of "foreach (ref x in c)" names an element of the range. A temporary range lives
    // as long as the loop, so it gets no entry.
    void track_loop_ref(const ForOfStmt& fo)
    {
        Place pl;
        if (is_cpp || !fo.range || !place_of(*fo.range, pl))
            return;
        pl.element = true;
        RefInfo info{pl, fo.pos, true};
        if (!fo.name.empty())
            refs[fo.name] = info;
        for (const std::string& n : fo.names)
            refs[n] = info;
    }

    // The name text of a declaration for a message: "r", or "(a, b)" for a structured binding.
    static std::string ref_names_text(const std::string& name, const std::vector<std::string>& names)
    {
        if (names.empty())
            return name;
        std::string t = "(";
        for (size_t i = 0; i < names.size(); ++i)
            t += (i ? ", " : "") + names[i];
        return t + ")";
    }

    void forget_ref(const std::string& name) { refs.erase(name); }

    // The type text of a parameter without its own "ref" modifier.
    static std::string param_inner(const Param& p)
    {
        if ((p.mode == ParamMode::Ref || p.mode == ParamMode::Out) && p.type.compare(0, 4, "ref ") == 0)
            return p.type.substr(4);
        return p.type;
    }

    // A "ref" word inside a stored type is an error (a ".hot" script only).
    void ref_storage(const std::string& type, uint32_t pos)
    {
        if (!is_cpp && has_ref_word(type))
            error(pos, kRefStorageError);
    }

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
        // A pointer to a data member ("int P::*") is not a borrowed pointer. It has an own message.
        if (is_cpp && what == "a field" && t.find("::*") != std::string::npos && t.find('(') == std::string::npos)
        {
            error(pos,
                "a field cannot be a pointer to a data member ('" + trim(t)
                    + "'): it is not supported in a .cpp script. Use a pointer to a member function, or an index");
            return;
        }
        if (is_borrowed_form(t, base))
        {
            if (base == "char")
                return; // "const char*": the text of a string literal
            if (script_pointer(t, pos))
                return;
            // A script object has a fixed address for its life: a reload moves only the state block.
            // So a return of "P&", "const P&" or "P*" of a script class is not a borrowed value.
            if (is_cpp && what == "a return value" && types.is_script_class(base) && is_single_ref_or_pointer(t))
                return;
            const TypeRule rule = types.lookup(base);
            // An intrusive stub: "T*" is the handle. A "T&" or a "T**" is still borrowed.
            if (rule.kind == Kind::Handle && !rule.intrusive_add.empty() && !types.is_script_class(base)
                && is_single_pointer(t))
                return;
            if (what == "a field" && field_pointer_rule(t, base, pos))
                return;
            const std::string name = script_type_text(base);
            if (is_cpp && what != "a field")
            { // the text for C++: a .cpp script has no stub with 'handle'
                borrowed_finding(pos,
                    what + " cannot hold a borrowed pointer or reference to '" + name
                        + "': the object can be gone when the caller uses it. Use a value or a smart pointer such as "
                          "'std::shared_ptr<" + name + ">'");
                return;
            }
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


    // True when the type is "T&", "const T&", "T*" or "const T*": one trailing sign and no other sign.
    static bool is_single_ref_or_pointer(const std::string& type)
    {
        const std::string t = trim(type);
        if (t.empty() || (t.back() != '*' && t.back() != '&'))
            return false;
        const std::string rest = trim(t.substr(0, t.size() - 1));
        return !rest.empty() && rest.back() != '*' && rest.back() != '&';
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
        error(pos, "a pointer to the script class '" + base + "' is not allowed: use '$" + base + "' (a handle)");
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
        method_ret = trim(m.ret);
        method_ret_ref = !is_cpp && is_ref_ret(m);
        method_ret_ptr = is_ptr_ret(m);
        body(m.params, m.body);
        method_ret.clear();
        method_ret_ref = false;
        method_ret_ptr = false;
        if (!is_cpp && !m.is_abstract)
            check_out_params(m.params, m.body, m.body.end_pos ? m.body.end_pos : m.pos,
                [&](uint32_t pos, const std::string& msg) { error(pos, msg); });
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
        owning_locals.clear();
        owning_stack.clear();
        view_src.clear();
        view_stack.clear();
        stale_views.clear();
        pending_stale.clear();
        stale_stack.clear();
        refs.clear();
        refs_stack.clear();
        ref_params.clear();
        copy_params.clear();
        for (const Param& p : params)
        {
            if (p.mode == ParamMode::Ref || p.mode == ParamMode::Out)
                ref_params.insert(p.name);
            else
                copy_params.insert(p.name);
            record_local(p.name, p.type, p.weak);
            if (!p.weak && !p.value && is_owning_container(p.type))
                owning_locals.insert(p.name); // a parameter by value owns its copy
            if (is_borrowed_type(p.type))
                add_borrowed(p.name, p.type);
        }
        walk_block(block);
    }

    // The hooks of the walk: a local, an owning name and a "ref" end with their block or statement.
    template <class W>
    void install_walk_hooks(W& w)
    {
        w.lambda_end = [&] { lambda_end(); };
        // A local ends at the end of its block: the owning names of the block do not leak out.
        w.block_begin = [&]
        {
            owning_stack.push_back(owning_locals);
            view_stack.push_back(view_src);
            stale_stack.push_back(stale_views);
            refs_stack.push_back(refs);
        };
        w.block_end = [&]
        {
            owning_locals = std::move(owning_stack.back());
            owning_stack.pop_back();
            view_src = std::move(view_stack.back());
            view_stack.pop_back();
            // A block that ends with a jump never reaches the code after it: its stale marks end too.
            if (last_stmt_exits)
                stale_views = stale_stack.back();
            stale_stack.pop_back();
            for (auto it = stale_views.begin(); it != stale_views.end();)
                it = view_src.count(it->first) ? std::next(it) : stale_views.erase(it);
            refs = std::move(refs_stack.back());
            refs_stack.pop_back();
        };
        // The variable of "foreach (ref x in c)" ends with the loop.
        w.stmt_end = [&](const Stmt& st)
        {
            for (auto& [n, cause] : pending_stale)
                if (view_src.count(n) && !stale_views.count(n))
                    stale_views[n] = std::move(cause);
            pending_stale.clear();
            last_stmt_exits = std::holds_alternative<ReturnStmt>(st.v) || std::holds_alternative<BreakStmt>(st.v)
                || std::holds_alternative<ContinueStmt>(st.v) || std::holds_alternative<ThrowStmt>(st.v);
            if (const auto* vd = std::get_if<VarDeclStmt>(&st.v))
                if (auto it = pending_refs.find(vd); it != pending_refs.end())
                {
                    for (auto& [n, info] : it->second)
                        refs[n] = std::move(info);
                    pending_refs.erase(it);
                }
            if (!iterating.empty() && iterating.back().stmt == &st)
                iterating.pop_back();
            if (const auto* fo = std::get_if<ForOfStmt>(&st.v))
            {
                refs.erase(fo->name);
                for (const std::string& n : fo->names)
                    refs.erase(n);
                if (auto it = hidden_refs.find(&st); it != hidden_refs.end())
                {
                    for (auto& [n, info] : it->second)
                        refs[n] = std::move(info);
                    hidden_refs.erase(it);
                }
            }
        };
    }

    void walk_block(const BlockStmt& b)
    {
        auto fe = [&](const Expr& e) { on_expr(e); };
        auto fs = [&](const Stmt& s) { on_stmt(s); };
        auto w = make_walker(fe, fs);
        install_walk_hooks(w);
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

    void record_local(const std::string& name, const std::string& type, bool weak = false)
    {
        locals.insert(name);
        local_types[name] = type;
        if (weak)
            weak_locals.insert(name);
        else
            weak_locals.erase(name);
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

    // True for the type "T[]" (a view): the text of the script or the C++ text.
    static bool is_view_type(const std::string& type)
    {
        std::string t = trim(type);
        if (!t.empty() && t[0] == '\x01') // the mark of the namespace "std"
            t.erase(0, 1);
        return (t.size() >= 2 && t.compare(t.size() - 2, 2, "[]") == 0) || t.rfind("std::span<", 0) == 0 ||
            t.rfind("hot::span<", 0) == 0 || t == "std::string_view";
    }

    // True for the type of a local that owns its elements: "std.vector<T>" and "T[N]".
    static bool is_owning_container(const std::string& type)
    {
        std::string t = trim(type);
        if (t.rfind("const ", 0) == 0)
            t = trim(t.substr(6));
        if (!t.empty() && t[0] == '\x01') // the mark of the namespace "std"
            t.erase(0, 1);
        if (t == "std::string")
            return true;
        // A pointer "std::vector<T>*" and a reference "std::vector<T>&" do not own the elements.
        if (t.empty() || t.back() != '>')
            return false;
        return t.rfind("std::vector<", 0) == 0 || t.rfind("std::array<", 0) == 0;
    }

    // The method of this class with a name, or null.
    const Method* own_method(const std::string& name) const
    {
        for (const Method& m : cd.methods)
            if (m.name == name)
                return &m;
        return nullptr;
    }

    // True when the value of the expression owns its elements: a new vector or array, a call of a
    // method of this class that returns one, or a copy of a local that owns its elements. The type of a
    // "var" local comes from its initializer.
    bool owning_init(const Expr& e) const
    {
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner && owning_init(*p->inner);
        if (auto* n = std::get_if<NameExpr>(&e.v))
            return n->parts.size() == 1 && n->template_args.empty() && owning_locals.count(n->parts[0]) != 0;
        if (auto* c = std::get_if<CallExpr>(&e.v); c && c->callee)
        {
            const Method* m = nullptr;
            if (auto* n = std::get_if<NameExpr>(&c->callee->v))
            {
                if (n->parts.size() == 2 && (n->parts[1] == "vector" || n->parts[1] == "array")
                    && (n->parts[0] == "std" || n->parts[0] == "\x01std"))
                    return true;
                if (n->parts.size() == 1 && n->template_args.empty() && !locals.count(n->parts[0]))
                    m = own_method(n->parts[0]);
            }
            else if (auto* mm = std::get_if<MemberExpr>(&c->callee->v); mm && mm->obj && !mm->safe
                     && std::holds_alternative<ThisExpr>(mm->obj->v))
                m = own_method(mm->name);
            return m && is_owning_container(m->ret);
        }
        return false;
    }

    // R4: "return e;" in a method with a "ref T" return. Every root of the place must outlive the call.
    void check_ref_return(const ReturnStmt& r) { check_return_place(*r.expr, r.pos, false); }

    // The place of a "ref" return, or the operand of "&" in a "T*" return ("ptr"). Every root of the place
    // must outlive the call.
    void check_return_place(const Expr& e, uint32_t pos, bool ptr)
    {
        const std::string what = ptr ? "a pointer" : "a 'ref'";
        Place pl;
        if (!place_of(e, pl))
        {
            error(pos, ptr ? "'return' of a pointer to a temporary: the value ends at the return. Return 'T' by value"
                           : "'return' of a temporary from a 'ref' method: the value ends at the return. Return 'T' by "
                             "value");
            return;
        }
        for (const std::string& root : pl.roots)
        {
            if (root == "this" || root.rfind("this.", 0) == 0 || ref_params.count(root))
                continue;
            if (copy_params.count(root))
                error(pos, "'return' of " + what + " to the parameter '" + root
                               + "': the parameter is a copy and ends at the return. Return a value, or take the "
                                 "parameter by 'ref'");
            else if (locals.count(root))
                error(pos, "'return' of " + what + " to the local '" + root + "': it ends at the return. Return a value, or "
                               + (ptr ? "a pointer" : "a ref") + " to a field or a 'ref' parameter");
        }
    }

    // "return e;" in a method with a "T*" return of a value struct. "e" is "null", "&place" or a choice of
    // these. The place of "&" follows the rules of a "ref" return.
    void check_ptr_return(const Expr& e, uint32_t pos)
    {
        if (const auto* p = std::get_if<ParenExpr>(&e.v))
        {
            if (p->inner)
                check_ptr_return(*p->inner, pos);
            return;
        }
        if (const auto* t = std::get_if<TernaryExpr>(&e.v))
        {
            for (const ExprP* side : {&t->a, &t->b})
                if (*side)
                    check_ptr_return(**side, pos);
            return;
        }
        if (std::holds_alternative<NullExpr>(e.v))
            return;
        if (const UnaryExpr* u = as_address_of(e); u && u->operand)
        {
            allowed_refs.insert(&e);
            check_return_place(*u->operand, pos, true);
            return;
        }
        error(pos, "a 'T*' return of a value struct must be 'null' or '&place', such as '&this.items[i]'. Return "
                   "the place, or a value");
    }

    // The rules of the declaration of a method with a "ref T" return (a ".hot" script only).
    void check_ref_return_decl(const Method& m)
    {
        if (is_cpp || !is_ref_ret(m))
            return;
        if (m.is_async)
            error(m.pos, "an 'async' method cannot return 'ref': the task outlives the call");
        std::string base = trim(is_ref_return_text(m.ret) ? strip_ref_return_word(m.ret) : m.ret);
        while (!base.empty() && (base.back() == '&' || base.back() == ' '))
            base.pop_back();
        if ((types.is_script_class(base) && !types.is_script_value(base)) || types.is_handle(base))
        {
            const std::string t = !base.empty() && base[0] == '$' ? base.substr(1) : base;
            error(m.pos, "'ref " + base + "' return is not allowed for a class type: return the handle '$" + t + "'");
        }
    }

    void on_stmt(const Stmt& s)
    {
        if (auto* r = std::get_if<ReturnStmt>(&s.v))
        {
            if (in_async_void && scopes.empty() && r->expr)
                error(r->pos, "an 'async void' method cannot return a value");
            if (method_ret_ref && scopes.empty() && r->expr)
                check_ref_return(*r);
            if (method_ret_ptr && scopes.empty() && r->expr)
                check_ptr_return(*r->expr, r->pos);
            // "return v;" in a method of the type "T[]": the view points into the local "v". The same holds
            // for a view local over such a local ("T[] s = v; return s;").
            if (scopes.empty() && r->expr && !is_cpp && is_view_type(method_ret))
            {
                std::string target; // the owner of the elements, as text for the message
                if (auto* n = std::get_if<NameExpr>(&r->expr->v))
                {
                    if (n->parts.size() == 1 && owning_locals.count(n->parts[0]))
                        target = "the local '" + n->parts[0] + "'";
                    else if (auto v = view_src.find(n->parts[0]);
                             n->parts.size() == 1 && v != view_src.end() && owning_locals.count(v->second))
                        target = "the local '" + v->second + "'";
                }
                else if (owning_init(*r->expr)) // "return std.vector<int>();"
                    target = "a temporary value";
                if (!target.empty())
                {
                    const bool text = trim(method_ret).back() != ']';
                    error(r->pos,
                        std::string("the view '") + (text ? "std.string_view" : "T[]") + "' points into " + target
                            + ", which ends at the return: return a " + (text ? "std.string" : "std.vector<T>")
                            + " instead");
                }
            }
        }
        if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            // The name can hide an outer local of another kind, so a name that does not own erases it.
            const bool owns = d->names.empty() && !d->is_ref
                && (d->type.empty() ? d->init && owning_init(*d->init) : is_owning_container(d->type));
            for (const std::string& n : d->names)
                owning_locals.erase(n);
            track_view(*d);
            if (d->names.empty())
            {
                if (owns)
                    owning_locals.insert(d->name);
                else
                    owning_locals.erase(d->name);
            }
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            owning_locals.erase(fo->name);
            for (const std::string& n : fo->names)
                owning_locals.erase(n);
        }
        if (auto* d = std::get_if<VarDeclStmt>(&s.v))
        {
            if (!d->names.empty())
            { // a structured binding: plain locals, "ref" borrows
                for (const std::string& n : d->names)
                {
                    record_local(n, std::string());
                    forget_ref(n);
                    if (d->is_ref)
                        borrowed.insert(n);
                }
                if (d->is_ref)
                    track_ref(*d);
                return;
            }
            record_local(d->name, d->type, d->weak);
            forget_ref(d->name);
            if (!d->type.empty())
            {
                script_pointer(d->type, d->pos);
                ref_storage(d->type, d->pos);
            }
            if (d->is_ref)
            {
                borrowed.insert(d->name); // "ref r = x": a local reference
                track_ref(*d);
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
            else if (const Method* cm = d->type.empty() && d->init ? called_method(*d->init) : nullptr;
                cm && is_ptr_ret(*cm))
            {
                add_borrowed(d->name, "*"); // "var p = c.Find(1)": a "T*" return of a value struct
            }
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            record_local(fo->name, fo->type);
            std::vector<std::string> loop_names{fo->name};
            loop_names.insert(loop_names.end(), fo->names.begin(), fo->names.end());
            for (const std::string& n : loop_names)
            {
                auto it = refs.find(n);
                if (it != refs.end())
                    hidden_refs[&s].emplace_back(n, it->second);
                forget_ref(n);
            }
            if (fo->is_ref)
                track_loop_ref(*fo);
            else if (!is_cpp && scopes.empty() && fo->range)
                if (std::string r = root_name(*fo->range); !r.empty())
                    iterating.push_back({&s, r});
            if (!fo->type.empty())
            {
                script_pointer(fo->type, fo->pos);
                ref_storage(fo->type, fo->pos);
            }
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

    // The root of a container in an expression: "this.xs" or the field "xs" gives "this.xs", and a local
    // "v" gives "v". Else the result is empty. The roots of the places use the same names.
    std::string root_name(const Expr& e) const
    {
        if (const auto* n = std::get_if<NameExpr>(&e.v))
            return n->parts.size() == 1 && n->template_args.empty() ? name_root(n->parts[0]) : std::string();
        if (const auto* m = std::get_if<MemberExpr>(&e.v))
            if (!m->safe && m->obj && std::holds_alternative<ThisExpr>(m->obj->v))
                return "this." + m->name;
        return std::string();
    }

    // The name of a root in a message: the field "this.xs" is written "xs".
    static std::string root_text(const std::string& r) { return r.rfind("this.", 0) == 0 ? r.substr(5) : r; }

    // R2 and the foreach rule: an operation that can change the shape of the container "r" while a "ref"
    // points to an element of it. "what" starts the message.
    void report_shape_change(uint32_t pos, const std::string& r, const std::string& what)
    {
        const std::string t = root_text(r);
        // A change of the container that a "foreach (var ...)" loop walks invalidates its iterators.
        if (scopes.empty())
            for (const IterLoop& it : iterating)
                if (it.root == r)
                    error(pos, what + " changes '" + t + "' inside the 'foreach' over '" + t
                                   + "': the loop can crash. Collect the changes in a new list, and apply them after the loop");
        if (scopes.empty())
            for (const auto& [n, src] : view_src)
                if (src == r && !stale_views.count(n))
                    pending_stale.emplace(n, what + " changes '" + t + "'");
        for (const auto& [n, info] : refs)
        {
            if (!info.place.element)
                continue;
            // The result of a call of a "ref" method can be inside any field of the object that can change.
            bool hit = info.place.shape_root(r);
            if (!hit && info.place.any_field && r.rfind("this.", 0) == 0)
            {
                auto f = field_types.find(t);
                hit = f != field_types.end() && can_reshape(f->second);
            }
            if (!hit)
                continue;
            if (info.loop)
                error(pos, what + " changes '" + t + "' inside 'foreach (ref " + n + " in " + t
                               + ")': the loop variable can dangle. Collect the changes in a new list, and apply "
                                 "them after the loop");
            else
                error(pos, what + " can move or remove the elements of '" + t + "' while the 'ref' '" + n
                               + "' points to one. Copy the value with 'var', or end the ref before the call");
        }
    }

    static bool is_assign_op(const std::string& op)
    {
        return !op.empty() && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=";
    }

    // The own method (a method or a static) of the class that a call names, with a "ref" or "out"
    // parameter at the position "i".
    bool own_ref_param(const std::string& name, size_t argc, size_t i) const
    {
        for (const std::vector<Method>* list : {&cd.methods, &cd.statics})
            for (const Method& m : *list)
                if (m.name == name && m.params.size() >= argc && i < m.params.size()
                    && (m.params[i].mode == ParamMode::Ref || m.params[i].mode == ParamMode::Out))
                    return true;
        return false;
    }

    // The name of the own method or static that a call names ("F()", "this.F()" or "Class.F()"), else empty.
    std::string own_callee_name(const CallExpr& c) const
    {
        if (const auto* cn = std::get_if<NameExpr>(&c.callee->v))
        {
            if (cn->parts.size() == 1)
                return cn->parts[0];
            if (cn->parts.size() == 2 && cn->parts[0] == cd.name)
                return cn->parts[1];
        }
        else if (const auto* cm = std::get_if<MemberExpr>(&c.callee->v))
        {
            if (!cm->safe && cm->obj && std::holds_alternative<ThisExpr>(cm->obj->v))
                return cm->name;
        }
        return std::string();
    }

    void check_invalidation(const Expr& e)
    {
        if (const auto* c = std::get_if<CallExpr>(&e.v); c && c->callee)
        {
            std::string r, method;
            // An argument at a "ref" or "out" parameter of an own method can change the shape of the container.
            if (const std::string own = own_callee_name(*c); !own.empty())
                for (size_t i = 0; i < c->args.size(); ++i)
                    if (c->args[i] && own_ref_param(own, c->args.size(), i))
                    {
                        const std::string a = root_name(*c->args[i]);
                        if (!a.empty())
                            report_shape_change(c->pos, a, "'" + root_text(a) + "' passed by 'ref'");
                    }
            if (const auto* cn = std::get_if<NameExpr>(&c->callee->v))
            {
                if (cn->parts.size() == 2)
                {
                    r = name_root(cn->parts[0]);
                    method = cn->parts[1];
                }
            }
            else if (const auto* cm = std::get_if<MemberExpr>(&c->callee->v))
            {
                if (!cm->safe && cm->obj)
                {
                    r = root_name(*cm->obj);
                    method = cm->name;
                }
            }
            if (!r.empty() && is_mutating_method(method))
                report_shape_change(c->pos, r, "'" + root_text(r) + "." + method + "()'");
        }
        else if (const auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (is_assign_op(b->op) && b->lhs)
            {
                const std::string r = root_name(*b->lhs);
                if (!r.empty())
                    report_shape_change(b->pos, r, "assignment to '" + root_text(r) + "'");
            }
        }
        else if (const UnaryExpr* u = as_address_of(e))
        {
            if (u->operand)
            {
                const std::string r = root_name(*u->operand);
                if (!r.empty())
                    report_shape_change(u->pos, r, "'" + root_text(r) + "' passed by 'ref'");
            }
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

    // A use of a view after its source changed shape. An assignment "s = x" gives the view a new source.
    void check_stale_view(const Expr& e)
    {
        if (const auto* b = std::get_if<BinaryExpr>(&e.v); b && b->op == "=" && b->lhs)
            if (const auto* l = std::get_if<NameExpr>(&b->lhs->v); l && l->parts.size() == 1)
                if (stale_views.erase(l->parts[0]))
                {
                    view_src.erase(l->parts[0]);
                    return;
                }
        if (const auto* n = std::get_if<NameExpr>(&e.v); n && !n->parts.empty())
            if (auto it = stale_views.find(n->parts[0]); it != stale_views.end())
            {
                error(n->pos, "the view '" + n->parts[0] + "' is used after " + it->second
                                  + ". The view can point to freed memory. Take the view again after the change, or use "
                                    "an index");
                stale_views.erase(it);
            }
    }

    void on_expr(const Expr& e)
    {
        if (!is_cpp && (!refs.empty() || !iterating.empty() || !view_src.empty()))
            check_invalidation(e);
        if (!stale_views.empty() && scopes.empty())
            check_stale_view(e);
        if (auto* un = std::get_if<UnaryExpr>(&e.v))
        {
            check_cast(*un);
            if (!un->postfix && un->op == "await" && !is_cpp)
            {
                if (!scopes.empty())
                    error(un->pos, "'await' is only valid inside an 'async' method (a lambda cannot await yet)");
                else if (!in_async)
                    error(un->pos, "'await' is only valid inside an 'async' method");
                else
                    for (const auto& r : refs)
                        error(un->pos, "the 'ref' '" + r.first
                                           + "' is live across 'await': the target can end before the task continues. "
                                             "Copy the value, or put the ref in a block that ends before the await");
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
            if (!declared.count(n) && !is_cpp && (refs.count(n) || ref_params.count(n)))
                error(l.pos, "the lambda captures the 'ref' '" + n
                                 + "'. The closure can outlive the target. Copy the value before the lambda, or "
                                   "capture a handle '$T'");
            else if (!declared.count(n) && borrowed.count(n))
                borrowed_finding(l.pos, "the lambda captures '" + n + "', a borrowed pointer or reference. "
                             "A closure cannot keep it. Copy the value or use a handle");
        // The parameters and the locals of the body live only to the end of the lambda.
        scopes.push_back({locals, local_types, borrowed, pointers});
        for (const Param& p : l.params)
        {
            record_local(p.name, p.type, p.weak);
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

    // True when the type holds a raw native pointer: "T*", or a template argument "T*" at any depth
    // ("std::vector<T*>", "std::map<K, T*>"). A "const char*" is the text of a literal. An intrusive
    // handle is a counted handle. A function type is not a stored pointer.
    bool holds_raw_pointer(const std::string& type) const
    {
        const std::string t = trim(type);
        if (t.empty() || t.find('(') != std::string::npos || t.find("::*") != std::string::npos)
            return false;
        std::string base;
        if (t.back() == '*' && is_borrowed_form(t, base))
        {
            if (base == "char")
                return false;
            const TypeRule rule = types.lookup(base);
            if (rule.kind == Kind::Handle && !rule.intrusive_add.empty() && !types.is_script_class(base))
                return false;
            return true;
        }
        for (const std::string& arg : template_args(t))
            if (holds_raw_pointer(arg))
                return true;
        return false;
    }

    // True when the text has the identifier "name". A comment and a string literal do not count.
    static bool text_uses_name(const std::string& text, const std::string& name)
    {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (size_t i = 0; i < text.size();)
        {
            const char c = text[i];
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
            {
                while (i < text.size() && text[i] != '\n')
                    ++i;
            }
            else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
            {
                const size_t e = text.find("*/", i + 2);
                i = e == std::string::npos ? text.size() : e + 2;
            }
            else if (c == '"' || c == '\'')
            {
                for (++i; i < text.size() && text[i] != c; ++i)
                    if (text[i] == '\\')
                        ++i;
                ++i;
            }
            else if (word(c))
            {
                size_t e = i;
                while (e < text.size() && word(text[e]))
                    ++e;
                if (text.compare(i, e - i, name) == 0 && e - i == name.size())
                    return true;
                i = e;
            }
            else
            {
                ++i;
            }
        }
        return false;
    }

    // True when the destructor body uses the field: "name" or "this.name".
    static bool dtor_uses(const Method& d, const std::string& name)
    {
        if (d.raw)
            return text_uses_name(d.raw_body, name);
        bool used = false;
        auto fe = [&](const Expr& e)
        {
            if (const auto* n = std::get_if<NameExpr>(&e.v))
            {
                if (!n->parts.empty() && n->parts[0] == name)
                    used = true;
                if (n->parts.size() > 1 && n->parts[0] == "this" && n->parts[1] == name)
                    used = true;
            }
            else if (const auto* m = std::get_if<MemberExpr>(&e.v))
            {
                if (m->name == name && m->obj && std::holds_alternative<ThisExpr>(m->obj->v))
                    used = true;
            }
        };
        auto fs = [&](const Stmt&) {};
        make_walker(fe, fs).block(d.body);
        return used;
    }

    // A field with a raw pointer, or a template with a raw pointer argument, needs a destructor that
    // uses the field. So the user handles the pointer and frees it when needed.
    void check_pointer_fields_in_dtor()
    {
        if (!cd.foreign_unit.empty() || cd.iface_parent)
            return;
        if (cd.is_value && cd.dtors.empty())
            return;
        for (const Field& f : cd.fields)
        {
            // A pointer to a value type is an error of the storage rule already.
            if (!holds_raw_pointer(f.type) || types.is_value_pointer(f.type))
                continue;
            // The hidden field of an auto property "T P { get; set; }" has the name "__p_P".
            const std::string name = f.name.rfind("__p_", 0) == 0 ? f.name.substr(4) : f.name;
            const std::string kind = cd.is_value ? "struct" : "class";
            if (cd.dtors.empty())
            {
                error(f.pos,
                    "the field '" + name + "' holds a raw pointer, but the " + kind + " '" + cd.key_name()
                        + "' has no destructor. Add a destructor that deletes the pointer, sets it to null or "
                          "passes it on");
                continue;
            }
            bool used = false;
            for (const Method& d : cd.dtors)
                used = used || dtor_uses(d, name);
            if (!used)
                error(f.pos,
                    "the field '" + name + "' holds a raw pointer, but the destructor of the " + kind + " '"
                        + cd.key_name()
                        + "' does not use it. Delete the pointer, set it to null or pass it on in the destructor");
        }
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
            if (f.weak)
                weak_fields.insert(f.name);
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
            if (!is_cpp && has_view_word(f.type) && !(f.init && std::holds_alternative<StringExpr>(f.init->v)))
                error(f.pos, "a view ('T[]' or 'std.string_view') cannot be stored in a field or a container: it can "
                             "outlive its source. Use std.vector<T> or std.string");
            if (f.init)
            {
                locals.clear();
                borrowed.clear();
                pointers.clear();
                local_types.clear();
                refs.clear();
                auto fe = [&](const Expr& e) { on_expr(e); };
                auto fs = [&](const Stmt& s) { on_stmt(s); };
                auto w = make_walker(fe, fs);
                install_walk_hooks(w);
                w.expr(*f.init);
            }
        }
        // A static method has the same checks as a method.
        for (const std::vector<Method>* list : {&cd.methods, &cd.statics})
            for (const Method& m : *list)
            {
                check_async(m);
                for (const Param& p : m.params)
                {
                    script_pointer(p.type, m.pos);
                    ref_storage(param_inner(p), m.pos);
                }
                // A "ref" return type is valid. The type after it is a stored type.
                ref_storage(is_ref_return_text(m.ret) ? strip_ref_return_word(m.ret) : m.ret, m.pos);
                // An override keeps the native signature. The script cannot change its return type.
                if (!m.overrides_virtual && !m.ret.empty())
                {
                    // The hidden field of an auto property reports the type, so skip the getter.
                    const bool getter = m.name.rfind("get_", 0) == 0 && field_names.count("__p_" + m.name.substr(4));
                    if (!getter && !(is_ref_ret(m) && !is_cpp) && !is_ptr_ret(m))
                        check_storage(m.ret, m.pos, "a return value");
                }
                check_ref_return_decl(m);
                body(m);
            }
        for (const Ctor& c : cd.ctors)
        {
            for (const Param& p : c.params)
            {
                script_pointer(p.type, c.pos);
                ref_storage(param_inner(p), c.pos);
            }
            body(c.params, c.body);
            if (!is_cpp)
                check_out_params(c.params, c.body, c.body.end_pos ? c.body.end_pos : c.pos,
                    [&](uint32_t pos, const std::string& msg) { error(pos, msg); });
        }
        for (const Method& d : cd.dtors)
            body(d);
        check_pointer_fields_in_dtor();
    }
};

} // namespace

void check_class(const ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag,
    bool strict_borrowed, bool is_cpp, bool std_below_20, const std::vector<ClassDef>* classes)
{
    Checker c{cd, types, map, diag, strict_borrowed, is_cpp, std_below_20, {}, false, {}, {}, {}, {}, {}, {}};
    c.classes = classes;
    c.run();
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

// True when the type text is a bare script class: a box of the class (hot::Value<C>). A struct,
// an interface, a handle "$C" and any other text are not. A reference is not bare. A "const" prefix is allowed.
bool is_box_type(const TypeModel& types, const std::string& type)
{
    std::string t = trim(type);
    if (starts_with(t, "const "))
        t = trim(t.substr(6));
    return !t.empty() && t[0] != '$' && types.is_script_class(t) && !types.is_script_value(t);
}

// The type of a reference without the "const" and the "&": "const C&" gives "C".
std::string referent_type(const std::string& type)
{
    std::string t = trim(type);
    while (!t.empty() && t.back() == '&')
        t = trim(t.substr(0, t.size() - 1));
    if (starts_with(t, "const "))
        t = trim(t.substr(6));
    return t;
}

// The rules for one box use of a script class. "what" names the construct, for example "field
// 'f'". "needs_ctor" is true for a declaration without an initializer: C must have a constructor
// with no parameters (a field default runs "new C()").
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

    void use(const std::string& type, uint32_t pos, const std::string& what, bool needs_ctor)
    {
        if (!is_box_type(types, type))
            return;
        const std::string t = referent_type(type);
        if (!types.script_native_base(t).empty())
        {
            diag.add(pos, map, kNativeBaseValueError);
            return;
        }
        const ClassDef* c = find(t);
        if (needs_ctor && c && !has_default_ctor(*c))
            diag.add(pos, map,
                "a value of the class '" + t + "' in " + what
                    + " has no initializer, so the class needs a constructor with no parameters. Add one, or write an "
                      "initializer");
    }

    // A bare script class in a template argument is a box too: "std.vector<C>" holds boxes.
    // A class with a native base cannot be a box, so the argument is an error.
    void targs(const std::string& type, uint32_t pos)
    {
        int depth = 0;
        for (size_t i = 0; i < type.size();)
        {
            const char ch = type[i];
            if (ch == '<')
                ++depth;
            else if (ch == '>')
                --depth;
            if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_'))
            {
                ++i;
                continue;
            }
            size_t e = i;
            while (e < type.size() && (std::isalnum(static_cast<unsigned char>(type[e])) || type[e] == '_' || type[e] == '.'))
                ++e;
            const bool marked = i > 0 && type[i - 1] == '$';
            if (depth > 0 && !marked && !types.script_native_base(type.substr(i, e - i)).empty())
            {
                diag.add(pos, map, kNativeBaseValueError);
                return;
            }
            i = e;
        }
    }

    void locals(const BlockStmt& body)
    {
        auto fe = [&](const Expr& x)
        {
            if (const auto* l = std::get_if<LambdaExpr>(&x.v))
                params(l->params, l->pos);
        };
        auto fs = [&](const Stmt& s)
        {
            const auto* d = std::get_if<VarDeclStmt>(&s.v);
            if (d)
                targs(d->type, d->pos);
            if (d && d->value)
                use(d->type, d->pos, "local '" + d->name + "'", d->init == nullptr);
            const auto* fo = std::get_if<ForOfStmt>(&s.v);
            if (fo)
                targs(fo->type, fo->pos);
            if (fo && fo->value)
                use(fo->type, fo->pos, "foreach variable '" + fo->name + "'", false);
        };
        make_walker(fe, fs).block(body);
    }

    // A "ref" or "out" parameter of a box type: the box has no reference form. An "in" parameter is the plain form.
    void params(const std::vector<Param>& ps, uint32_t pos)
    {
        for (const Param& p : ps)
        {
            targs(p.type, pos);
            if (p.value)
                use(p.type, pos, "parameter '" + p.name + "'", false);
            else if (p.mode != ParamMode::Plain && p.mode != ParamMode::ByVal && p.mode != ParamMode::In && !p.weak
                && is_box_type(types, referent_type(p.type)))
                diag.add(pos, map,
                    "a reference to a value of a script class is not supported; write $" + referent_type(p.type));
        }
    }

    void ref_storage(const std::string& type, uint32_t pos)
    {
        if (has_ref_word(type))
            diag.add(pos, map, kRefStorageError);
    }

    void run()
    {
        for (const ClassDef& cd : classes)
        {
            for (const Property& p : cd.props)
                ref_storage(p.type, p.pos);
            for (const Field& f : cd.fields)
            {
                ref_storage(f.type, f.pos);
                targs(f.type, f.pos);
                if (!f.value)
                    continue;
                if (cd.is_value)
                    diag.add(f.pos, map,
                        cd.name == kModuleClassName
                            ? "a module variable cannot hold a value of a script class; write $" + trim(f.type) + " "
                                + f.name
                            : "the field '" + f.name + "' of the struct '" + cd.name
                                + "' cannot hold a value of a script class; write $" + trim(f.type) + " " + f.name);
                else
                    use(f.type, f.pos, "field '" + f.name + "'", !f.has_init);
            }
            for (const std::vector<Method>* list : {&cd.methods, &cd.statics})
                for (const Method& m : *list)
                {
                    params(m.params, m.pos);
                    targs(m.ret, m.pos);
                    if (m.ret_value)
                        use(m.ret, m.pos, "the return type of '" + m.name + "'", false);
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

// Sets the flag of a box use on each declaration of the script whose type is a bare script class.
struct ValueResolver
{
    const TypeModel& types;

    // A "ref" or "out" parameter and a weak parameter get no flag. An "in" parameter of a box
    // type is the plain form: the parser wrote "const C&", and the type becomes "C". The box
    // parameter is "hot::In<hot::Value<C>>", which is a const reference.
    void params_of(Param& p)
    {
        if (p.mode == ParamMode::In && !p.weak && is_box_type(types, referent_type(p.type)))
        {
            p.type = referent_type(p.type);
            p.value = true;
            return;
        }
        const bool plain = p.mode == ParamMode::Plain || p.mode == ParamMode::ByVal;
        p.value = plain && !p.weak && is_box_type(types, p.type);
    }

    void fn_types(Method& m)
    {
        for (Param& p : m.params)
            params_of(p);
        m.ret_value = is_box_type(types, m.ret);
    }

    void locals(BlockStmt& body)
    {
        // The walker reads the tree as const. This pass owns the tree and sets the flags.
        auto fe = [&](const Expr& x)
        {
            if (const auto* l = std::get_if<LambdaExpr>(&x.v))
                for (Param& p : const_cast<LambdaExpr*>(l)->params)
                    params_of(p);
        };
        auto fs = [&](const Stmt& s)
        {
            auto* d = std::get_if<VarDeclStmt>(&const_cast<Stmt&>(s).v);
            if (d && !d->weak && !d->is_ref && d->names.empty() && is_box_type(types, d->type))
                d->value = true;
            auto* fo = std::get_if<ForOfStmt>(&const_cast<Stmt&>(s).v);
            if (fo && !fo->is_ref && fo->names.empty() && is_box_type(types, fo->type))
                fo->value = true;
        };
        make_walker(fe, fs).block(body);
    }

    void run(ClassDef& cd)
    {
        for (Field& f : cd.fields)
            f.value = !f.weak && is_box_type(types, f.type);
        for (std::vector<Method>* list : {&cd.methods, &cd.statics})
            for (Method& m : *list)
            {
                fn_types(m);
                locals(m.body);
            }
        for (Ctor& c : cd.ctors)
        {
            for (Param& p : c.params)
                params_of(p);
            locals(c.body);
        }
        for (Method& d : cd.dtors)
            locals(d.body);
    }

    void run(InterfaceDef& d)
    {
        for (Method& m : d.methods)
            fn_types(m);
    }
};

} // namespace

void resolve_value_uses(ClassDef& cd, const TypeModel& types)
{
    ValueResolver{types}.run(cd);
}

void resolve_value_uses(InterfaceDef& d, const TypeModel& types)
{
    ValueResolver{types}.run(d);
}

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

namespace
{
// A call by the identifier of an operator ("a.op_add(b)"). The script writes "a + b". The check does
// not know the type of the object: any member call with such a name is an error. "syms" holds the
// symbols that the own class defines: only these have the call "op_add(b)" with no object.
auto operator_call_check(std::set<std::string> syms, const LineMap& map, Diagnostics& diag)
{
    return [syms = std::move(syms), &map, &diag](const Expr& e)
    {
        const auto* c = std::get_if<CallExpr>(&e.v);
        if (!c || !c->callee)
            return;
        std::string name;
        uint32_t pos = c->pos;
        if (const auto* me = std::get_if<MemberExpr>(&c->callee->v))
        {
            name = me->name;
            pos = me->name_pos;
        }
        else if (const auto* ne = std::get_if<NameExpr>(&c->callee->v))
        {
            if (ne->parts.size() > 1)
                name = ne->parts.back();
            else if (!ne->parts.empty() && syms.count(op_symbol(ne->parts[0])))
                name = ne->parts[0];
            if (ne->last_pos)
                pos = ne->last_pos;
        }
        const std::string sym = op_symbol(name);
        if (!sym.empty())
        {
            const bool binary = is_binary_op_ident(name) || is_compare_op_ident(name);
            const std::string use = binary ? "write 'a " + sym + " b'"
                : name == "op_index"       ? "write 'a[i]'"
                                           : "write '" + sym + "a'";
            diag.add(pos, map, "'" + name + "' is an operator; " + use);
        }
    };
}

// True when the field type is stored in the object and a write through "f[i]" or "f.g" changes the
// object: a C array or a standard container. A handle, a box or a struct field is not known here.
bool field_holds_elements(const Field& f)
{
    const std::string& t = f.script_type.empty() ? f.type : f.script_type;
    if (t.find('$') != std::string::npos || t.find('*') != std::string::npos)
        return false;
    if (t.size() >= 2 && t.back() == ']' && std::isdigit(static_cast<unsigned char>(t[t.size() - 2])))
        return true;
    for (const char* p : {"std::vector<", "std::array<", "std::map<", "std::unordered_map<", "std::string"})
        if (t.rfind(p, 0) == 0)
            return true;
    return false;
}

// True when the body of "m", or the body of a function of the class that it calls, writes a field.
bool writes_field(const ClassDef& cd, const Method& m, std::set<const Method*>& seen)
{
    if (!seen.insert(&m).second)
        return false;
    const DirectWrites w = direct_writes(m);
    for (const auto& [name, exact] : w.roots)
        for (const Field& f : cd.fields)
            if (f.name == name && (exact || field_holds_elements(f)))
                return true;
    for (const std::string& call : w.calls)
        for (const Method& c : cd.methods)
            if (c.op.empty() && c.name == call && writes_field(cd, c, seen))
                return true;
    return false;
}
} // namespace

void check_operators(const ClassDef& cd, const LineMap& map, Diagnostics& diag)
{
    std::set<std::string> syms;
    for (const Method& m : cd.methods)
        if (!m.op.empty())
            syms.insert(m.op);
    static const std::set<std::string> compound = {"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    for (const Method& m : cd.methods)
    {
        if (m.op.empty())
        {
            // The identifier of an operator is the name of the member in the generated code.
            if (const std::string sym = op_symbol(m.name); !sym.empty())
                diag.add(m.pos, map, "'" + m.name + "' is reserved for an operator; write 'operator" + sym + "'");
            continue;
        }
        const std::string& sym = m.op;
        const std::string head = "operator '" + sym + "'";
        const size_t n = m.params.size();
        // The symbol: only the operators of the table are allowed.
        if (!op_ident(sym, 0) && !op_ident(sym, 1))
        {
            if (sym == "!=")
                diag.add(m.pos, map, head + " is generated from 'operator=='; remove it");
            else if (compound.count(sym))
                diag.add(m.pos, map,
                    head + " is generated from 'operator" + sym.substr(0, sym.size() - 1) + "'; remove it");
            else
                diag.add(m.pos, map, head + " cannot be declared in a script");
            continue;
        }
        // The parameter count.
        const bool unary_only = sym == "!" || sym == "~";
        const bool both = sym == "+" || sym == "-";
        if (unary_only && n != 0)
            diag.add(m.pos, map, head + " takes no parameter");
        else if (both && n > 1)
            diag.add(m.pos, map, head + " takes no parameter or one parameter");
        else if (!unary_only && !both && n != 1)
            diag.add(m.pos, map, head + " takes one parameter");
        // The return type.
        if (sym == "==" || sym == "<" || sym == ">" || sym == "<=" || sym == ">=")
        {
            if (m.ret != "bool")
                diag.add(m.pos, map, head + " must return bool");
        }
        else if (m.ret.empty() || m.ret == "void")
        {
            diag.add(m.pos, map, head + " cannot return void");
        }
        // The generated member of a compare or of an operator with a value return is const. It
        // casts the const away to call the function, so a write to a field is wrong for an object
        // that is const. A "ref" return gives a non-const member: it can write.
        if (!m.ret.empty() && (is_compare_op_ident(m.name) || !(m.ret.compare(0, 4, "ref ") == 0 || is_ref_type(m.ret))))
        {
            std::set<const Method*> seen;
            if (writes_field(cd, m, seen))
                diag.add(m.pos, map, head + " cannot change the object; return a new value instead");
        }
        // The ordering pair: C# rule.
        static const std::pair<const char*, const char*> pairs[] = {{"<", ">"}, {">", "<"}, {"<=", ">="}, {">=", "<="}};
        for (const auto& pr : pairs)
            if (sym == pr.first && !syms.count(pr.second))
                diag.add(m.pos, map, head + " needs 'operator" + pr.second + "'");
    }
    auto fe = operator_call_check(syms, map, diag);
    auto fs = [](const Stmt&) {};
    auto w = make_walker(fe, fs);
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

void check_operator_vars(const std::vector<ast::Field>& vars, const LineMap& map, Diagnostics& diag)
{
    auto fe = operator_call_check({}, map, diag);
    auto fs = [](const Stmt&) {};
    auto w = make_walker(fe, fs);
    for (const Field& f : vars)
        w.expr(f.init);
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
                if (tm->is_virtual || tm->is_override || tm->is_abstract)
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
            // "T& r = p;" and "ref r = p;" bind a writable alias to the parameter.
            if (auto* d = std::get_if<VarDeclStmt>(&s.v))
                if (d->init && (is_ref_type(d->type) || d->is_ref)
                    && rooted(d->init))
                {
                    use.mutated = true;
                    if (is_exactly(*d->init, name))
                        use.rebound = true;
                }
            // "foreach (ref e in p)" binds a writable alias to an element of the parameter.
            if (auto* fo = std::get_if<ForOfStmt>(&s.v))
                if (fo->is_ref && rooted(fo->range))
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
        std::string t = trim(type);
        if (!t.empty() && t[0] == '$')
            t = trim(t.substr(1)); // "$C" is the handle of the class "C"
        std::string base = t;
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
        for (const std::vector<Field>* list : {&sc.vars, &sc.consts})
            for (const Field& v : *list)
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

    // The type without a trailing "&" or "&&" and without the "const" in front. The probe asks about
    // the class: a "const std::vector<int>&" would give "T*" of a reference in the facts.
    static std::string without_reference(const std::string& cpp)
    {
        std::string t = trim(cpp);
        while (!t.empty() && t.back() == '&')
            t.pop_back();
        t = trim(t);
        if (t.rfind("const ", 0) == 0)
            t = trim(t.substr(6));
        return t;
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
                value_params[p.name] = without_reference(q.cpp);
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
            // A static method has no object: a bare name is never a member of the class.
            allow_this = false;
            for (const Method& m : c.statics)
                body(m.params, m.body);
            allow_this = true;
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
        // A free function has no class and no object.
        cls = nullptr;
        allow_this = false;
        for (const Method& m : sc.fns)
            body(m.params, m.body);
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

namespace
{
// The handle mark "$" does not belong to the class name.
std::string strip_handle_mark(const std::string& t)
{
    return !t.empty() && t[0] == '$' ? t.substr(1) : t;
}
} // namespace

std::set<std::string> block_new_targets(const ClassDef& cd)
{
    std::set<std::string> out;
    auto fe = [&](const Expr& e)
    {
        if (auto* n = std::get_if<NewExpr>(&e.v))
            out.insert(strip_handle_mark(n->type));
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
            out.insert(strip_handle_mark(n->type));
    };
    auto fs = [](const Stmt&) {};
    auto w = make_walker(fe, fs);
    for (const ClassDef& cd : sc.classes)
    {
        for (const Field& f : cd.fields)
            w.expr(f.init);
        for (const Method& m : cd.methods)
            w.block(m.body);
        for (const Method& m : cd.statics)
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
    for (const Method& m : sc.fns)
        w.block(m.body);
    return out;
}

namespace
{
// The type names of a class and of a list of free functions. Each one can be null.
std::set<std::string> collect_type_names(const ClassDef* cdp, const std::vector<Method>* fns)
{
    static const ClassDef none;
    const ClassDef& cd = cdp ? *cdp : none;
    std::set<std::string> out;
    // Also the type behind a "*" or "&", and the template arguments: the check of a
    // borrowed pointer asks for the rule of the type behind it.
    std::function<void(std::string)> add = [&](std::string t)
    {
        if (t.empty())
            return;
        t = strip_handle_mark(t);
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
    auto method = [&](const Method& m)
    {
        add(m.ret);
        for (const Param& p : m.params)
            add(p.type);
        w.block(m.body);
    };
    for (const Method& m : cd.methods)
        method(m);
    for (const Method& m : cd.statics)
        method(m);
    if (fns)
        for (const Method& m : *fns)
            method(m);
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

} // namespace

std::set<std::string> used_type_names(const ClassDef& cd)
{
    return collect_type_names(&cd, nullptr);
}

std::set<std::string> used_type_names(const Script& sc)
{
    std::set<std::string> out = collect_type_names(nullptr, &sc.fns);
    for (const ClassDef& cd : sc.classes)
    {
        std::set<std::string> one = used_type_names(cd);
        out.insert(one.begin(), one.end());
    }
    return out;
}

} // namespace hotc
