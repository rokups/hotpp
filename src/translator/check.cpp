// The checks of the script AST. See check.h.
#include "check.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

namespace sppc {

namespace {

using namespace ast;

// ------------------------------------------------------------------ walker
// Visits the nodes in the order of the script text, parent first. The walk goes into the
// bodies of lambdas. "on_expr" and "on_stmt" are called for each node.
template <class FE, class FS>
struct Walker {
    FE& on_expr;
    FS& on_stmt;

    void expr(const ExprP& e)
    {
        if (e) expr(*e);
    }

    void expr(const Expr& e)
    {
        on_expr(e);
        if (auto* m = std::get_if<MemberExpr>(&e.v)) {
            expr(m->obj);
        } else if (auto* c = std::get_if<CallExpr>(&e.v)) {
            expr(c->callee);
            for (const ExprP& a : c->args) expr(a);
        } else if (auto* ix = std::get_if<IndexExpr>(&e.v)) {
            expr(ix->obj);
            expr(ix->index);
        } else if (auto* u = std::get_if<UnaryExpr>(&e.v)) {
            expr(u->operand);
        } else if (auto* r = std::get_if<RefExpr>(&e.v)) {
            expr(r->operand);
        } else if (auto* b = std::get_if<BinaryExpr>(&e.v)) {
            expr(b->lhs);
            expr(b->rhs);
        } else if (auto* t = std::get_if<TernaryExpr>(&e.v)) {
            expr(t->cond);
            expr(t->a);
            expr(t->b);
        } else if (auto* p = std::get_if<ParenExpr>(&e.v)) {
            expr(p->inner);
        } else if (auto* n = std::get_if<NewExpr>(&e.v)) {
            for (const ExprP& a : n->args) expr(a);
        } else if (auto* l = std::get_if<LambdaExpr>(&e.v)) {
            block(l->body);
        }
    }

    void block(const BlockStmt& b)
    {
        for (const StmtP& s : b.stmts) stmt(s);
    }

    void stmt(const StmtP& s)
    {
        if (s) stmt(*s);
    }

    void stmt(const Stmt& s)
    {
        on_stmt(s);
        if (auto* b = std::get_if<BlockStmt>(&s.v)) {
            block(*b);
        } else if (auto* es = std::get_if<ExprStmt>(&s.v)) {
            expr(es->expr);
        } else if (auto* d = std::get_if<VarDeclStmt>(&s.v)) {
            expr(d->init);
        } else if (auto* i = std::get_if<IfStmt>(&s.v)) {
            expr(i->cond);
            stmt(i->then);
            stmt(i->otherwise);
        } else if (auto* w = std::get_if<WhileStmt>(&s.v)) {
            expr(w->cond);
            stmt(w->body);
        } else if (auto* dw = std::get_if<DoWhileStmt>(&s.v)) {
            stmt(dw->body);
            expr(dw->cond);
        } else if (auto* f = std::get_if<ForStmt>(&s.v)) {
            for (const StmtP& x : f->init) stmt(x);
            expr(f->cond);
            for (const ExprP& x : f->step) expr(x);
            stmt(f->body);
        } else if (auto* fo = std::get_if<ForOfStmt>(&s.v)) {
            expr(fo->range);
            stmt(fo->body);
        } else if (auto* r = std::get_if<ReturnStmt>(&s.v)) {
            expr(r->expr);
        } else if (auto* sw = std::get_if<SwitchStmt>(&s.v)) {
            expr(sw->subject);
            for (const SwitchCase& c : sw->cases) {
                for (const ExprP& l : c.labels) expr(l);
                for (const StmtP& x : c.stmts) stmt(x);
            }
        } else if (auto* t = std::get_if<ThrowStmt>(&s.v)) {
            expr(t->expr);
        } else if (auto* tr = std::get_if<TryStmt>(&s.v)) {
            block(tr->block);
            for (const CatchClause& c : tr->catches) block(c.block);
            if (tr->finally_block) block(*tr->finally_block);
        }
    }
};

template <class FE, class FS>
Walker<FE, FS> make_walker(FE& fe, FS& fs)
{
    return Walker<FE, FS>{fe, fs};
}

// ------------------------------------------------------------------ type text
std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

bool starts_with(const std::string& s, const char* p)
{
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

// The "ref" or "out" word before a type. Gives true and removes the word.
bool strip_pass_word(std::string& t)
{
    t = trim(t);
    for (const char* w : {"ref ", "out "})
        if (starts_with(t, w)) {
            t = trim(t.substr(4));
            return true;
        }
    return false;
}

// A trailing "*" or "&" of the type. Gives the type without it in "base".
bool is_borrowed_form(const std::string& type, std::string& base)
{
    std::string t = trim(type);
    if (t.empty() || (t.back() != '*' && t.back() != '&')) return false;
    while (!t.empty() && (t.back() == '*' || t.back() == '&' || t.back() == ' ')) t.pop_back();
    if (starts_with(t, "const ")) t = trim(t.substr(6));
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
    if (open == std::string::npos || close == std::string::npos || close < open) return out;
    int depth = 0;
    std::string cur;
    for (size_t i = open + 1; i < close; ++i) {
        const char c = type[i];
        if (c == '<' || c == '(') ++depth;
        if (c == '>' || c == ')') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

// ------------------------------------------------------------------ the checker
struct Checker {
    const ClassDef& cd;
    const TypeModel& types;
    const LineMap& map;
    Diagnostics& diag;
    bool strict;  // true: a borrowed-storage finding is an error. False: a warning.

    std::set<std::pair<uint32_t, std::string>> reported;
    std::set<std::string> fn_names, field_names;
    std::set<std::string> locals;    // parameters and locals of the body in progress
    std::set<std::string> borrowed;  // the locals that hold a borrowed pointer or a reference
    std::set<const Expr*> allowed_refs;   // "ref e" nodes in a place where they are valid
    std::set<const Expr*> callee_names;   // names in the callee place of a call

    void error(uint32_t pos, const std::string& msg)
    {
        if (reported.insert({pos, msg}).second) diag.add(pos, map, msg);
    }

    // A finding of the borrowed-storage rule (7.10.4 and 7.10.6).
    void borrowed_finding(uint32_t pos, const std::string& msg)
    {
        if (strict) error(pos, msg);
        else if (reported.insert({pos, msg}).second) diag.warn(pos, map, msg);
    }

    // ---- the storage rule (7.10.4 and 7.10.6)
    // "what" names the place: "a field", "a return value", "a collection element".
    void check_storage(const std::string& type, uint32_t pos, const std::string& what)
    {
        std::string t = type;
        if (strip_pass_word(t)) {
            borrowed_finding(pos, "a 'ref' or 'out' value cannot be " + what + ". A reload moves the state block, "
                       "so a stored reference would dangle. Use 'ref' only for a parameter or a local");
            return;
        }
        std::string base;
        if (is_borrowed_form(t, base)) {
            if (base == "char") return;  // "const char*": the text of a string literal
            if (types.lookup(base).kind == Kind::Handle) return;
            borrowed_finding(pos, what + " cannot hold a borrowed pointer or reference to '" + base +
                           "'. Declare a stub with 'handle' or 'intrusive' for '" + base +
                           "', or use a value");
            return;
        }
        for (const std::string& arg : template_args(t)) check_storage(arg, pos, "a collection element");
    }

    // True when a local of this declared type holds a borrowed pointer or a reference.
    static bool is_borrowed_type(const std::string& type)
    {
        std::string t = type, base;
        if (strip_pass_word(t)) return true;
        return is_borrowed_form(t, base) && base != "char";
    }

    // ---- one body
    void body(const Method& m)
    {
        locals.clear();
        borrowed.clear();
        for (const Param& p : m.params) {
            locals.insert(p.name);
            if (p.mode != PassMode::Value || is_borrowed_type(p.type)) borrowed.insert(p.name);
        }
        walk_block(m.body);
    }

    void walk_block(const BlockStmt& b)
    {
        auto fe = [&](const Expr& e) { on_expr(e); };
        auto fs = [&](const Stmt& s) { on_stmt(s); };
        auto w = make_walker(fe, fs);
        w.block(b);
    }

    void on_stmt(const Stmt& s)
    {
        if (auto* d = std::get_if<VarDeclStmt>(&s.v)) {
            locals.insert(d->name);
            if (d->init && std::holds_alternative<RefExpr>(d->init->v)) {
                allowed_refs.insert(d->init.get());
                borrowed.insert(d->name);
            } else if (!d->type.empty() && is_borrowed_type(d->type)) {
                borrowed.insert(d->name);
            }
        } else if (auto* fo = std::get_if<ForOfStmt>(&s.v)) {
            locals.insert(fo->name);
        } else if (auto* tr = std::get_if<TryStmt>(&s.v)) {
            for (const CatchClause& c : tr->catches) locals.insert(c.name);
        }
    }

    void on_expr(const Expr& e)
    {
        if (auto* c = std::get_if<CallExpr>(&e.v)) {
            callee_names.insert(c->callee.get());
            for (const ExprP& a : c->args)
                if (a && std::holds_alternative<RefExpr>(a->v)) allowed_refs.insert(a.get());
        } else if (auto* n = std::get_if<NewExpr>(&e.v)) {
            for (const ExprP& a : n->args)
                if (a && std::holds_alternative<RefExpr>(a->v)) allowed_refs.insert(a.get());
        } else if (auto* r = std::get_if<RefExpr>(&e.v)) {
            if (!allowed_refs.count(&e))
                borrowed_finding(r->pos, "a 'ref' value cannot be stored in an assignment, a 'return' or a "
                              "field initializer. Use 'ref' only for a call argument or a local");
        } else if (auto* nm = std::get_if<NameExpr>(&e.v)) {
            // 5.3: the script takes the address of a script function.
            if (nm->parts.size() == 1 && nm->template_args.empty() && fn_names.count(nm->parts[0]) &&
                !locals.count(nm->parts[0]) && !field_names.count(nm->parts[0]) && !callee_names.count(&e))
                error(nm->pos, "script function '" + nm->parts[0] +
                                   "' must be called. A script function has no address, because a reload "
                                   "replaces it");
        } else if (auto* l = std::get_if<LambdaExpr>(&e.v)) {
            lambda(*l);
        }
    }

    // The lambda captures by value. A captured borrowed pointer or reference would outlive the call.
    void lambda(const LambdaExpr& l)
    {
        std::set<std::string> used, declared;
        for (const Param& p : l.params) declared.insert(p.name);
        auto fe = [&](const Expr& e) {
            if (auto* n = std::get_if<NameExpr>(&e.v))
                if (!n->parts.empty()) used.insert(n->parts[0]);
            if (auto* inner = std::get_if<LambdaExpr>(&e.v))
                for (const Param& p : inner->params) declared.insert(p.name);
        };
        auto fs = [&](const Stmt& s) {
            if (auto* d = std::get_if<VarDeclStmt>(&s.v)) declared.insert(d->name);
            if (auto* fo = std::get_if<ForOfStmt>(&s.v)) declared.insert(fo->name);
        };
        make_walker(fe, fs).block(l.body);
        for (const std::string& n : used)
            if (!declared.count(n) && borrowed.count(n))
                borrowed_finding(l.pos, "the lambda captures '" + n + "', a borrowed pointer or reference. "
                             "A closure cannot keep it. Copy the value or use a handle");
        for (const Param& p : l.params) {
            locals.insert(p.name);
            if (p.mode != PassMode::Value || is_borrowed_type(p.type)) borrowed.insert(p.name);
        }
    }

    void run()
    {
        for (const Method& m : cd.methods)
            if (!m.overrides_virtual) fn_names.insert(m.name);
        for (const Field& f : cd.fields) field_names.insert(f.name);

        for (const Field& f : cd.fields) {
            check_storage(f.type, f.pos, "a field");
            if (f.init) {
                locals.clear();
                borrowed.clear();
                auto fe = [&](const Expr& e) { on_expr(e); };
                auto fs = [&](const Stmt& s) { on_stmt(s); };
                make_walker(fe, fs).expr(*f.init);
            }
        }
        for (const Method& m : cd.methods) {
            // An override keeps the native signature. The script cannot change its return type.
            if (!m.overrides_virtual && !m.ret.empty()) {
                // The hidden field of an auto property reports the type, so skip the getter.
                const bool getter = m.name.rfind("get_", 0) == 0 && field_names.count("__p_" + m.name.substr(4));
                if (!getter) check_storage(m.ret, m.pos, "a return value");
            }
            body(m);
        }
    }
};

}  // namespace

void check_class(const ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag,
                 bool strict_borrowed)
{
    Checker{cd, types, map, diag, strict_borrowed, {}, {}, {}, {}, {}, {}, {}}.run();
}

std::set<std::string> used_type_names(const Script& sc)
{
    std::set<std::string> out;
    // Also the type behind a "*" or "&", and the template arguments: the check of a
    // borrowed pointer asks for the rule of the type behind it.
    std::function<void(std::string)> add = [&](std::string t) {
        strip_pass_word(t);
        if (t.empty()) return;
        out.insert(t);
        std::string base;
        if (is_borrowed_form(t, base)) add(base);
        else
            for (const std::string& a : template_args(t)) add(a);
    };
    auto fe = [&](const Expr& e) {
        if (auto* n = std::get_if<NewExpr>(&e.v)) add(n->type);
        if (auto* l = std::get_if<LambdaExpr>(&e.v))
            for (const Param& p : l->params) add(p.type);
    };
    auto fs = [&](const Stmt& s) {
        if (auto* d = std::get_if<VarDeclStmt>(&s.v)) add(d->type);
        if (auto* fo = std::get_if<ForOfStmt>(&s.v)) add(fo->type);
    };
    auto w = make_walker(fe, fs);
    for (const ClassDef& cd : sc.classes) {
        for (const Field& f : cd.fields) {
            add(f.type);
            w.expr(f.init);
        }
        for (const Method& m : cd.methods) {
            add(m.ret);
            for (const Param& p : m.params) add(p.type);
            w.block(m.body);
        }
        for (const Property& p : cd.props) {
            add(p.type);
            w.expr(p.init);
            if (p.get_body) w.block(*p.get_body);
            if (p.set_body) w.block(*p.set_body);
        }
    }
    return out;
}

}  // namespace sppc
