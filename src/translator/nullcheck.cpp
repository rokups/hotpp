// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The nullability analyzer. See nullcheck.h for the rules.
//
// The pass keeps a "State" for the current path: the set of variables known as non null, and a
// flag that says if the path is alive. The merge of two paths keeps a variable only when each
// live path has it. This is the same structure as "PathWalk" in check.cpp (definite assignment):
// a "return", a "throw", a "break" and a "continue" end the path, and a loop body can run zero times.
// "PathWalk" tracks one name with one flag. This pass tracks many names, and a condition gives two
// states (the true path and the false path), so it has its own walk.
#include "nullcheck.h"

#include "ast_walk.h"

#include <map>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace hotc
{

namespace
{

using namespace ast;

// ------------------------------------------------------------------ type text
std::string trim(const std::string& s)
{
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

bool starts_with(const std::string& s, const char* p)
{
    return s.compare(0, std::char_traits<char>::length(p), p) == 0;
}

// The type without a "const", a mode word and a "&".
std::string strip_type(const std::string& raw)
{
    std::string t = trim(raw);
    for (bool again = true; again;)
    {
        again = false;
        for (const char* w : {"const ", "in ", "ref ", "out "})
            if (starts_with(t, w))
            {
                t = trim(t.substr(std::char_traits<char>::length(w)));
                again = true;
            }
    }
    while (!t.empty() && t.back() == '&')
        t.pop_back();
    return trim(t);
}

// "T*", "$T" and "T?" can hold null.
bool ptr_like(const std::string& raw)
{
    const std::string t = strip_type(raw);
    return !t.empty() && (t[0] == '$' || t.back() == '*' || t.back() == '?');
}

// The class name of a type text: "$C", "C*" and "C?" give "C".
std::string class_of(const std::string& raw)
{
    std::string t = strip_type(raw);
    if (!t.empty() && t[0] == '$')
        t.erase(0, 1);
    while (!t.empty() && (t.back() == '*' || t.back() == '?'))
        t.pop_back();
    return trim(t);
}

template <class T> const std::string& stype(const T& x)
{
    return x.script_type.empty() ? x.type : x.script_type;
}

// ------------------------------------------------------------------ values and states
enum class Origin
{
    Other,
    Local,
    Field
};

// What the pass knows about the value of an expression.
struct Val
{
    std::string type; // the type text, empty when not known
    bool ptr = false; // the type is "T*", "$T", "T?" or a native pointer or handle
    bool nonnull = false; // the value is known as non null (only with "ptr")
    bool cnst = false; // the type text has a "const": a cast to the same type text is not a cast to the same type
    Origin origin = Origin::Other;
    std::string text; // the text of a simple name or of a chain of names, else empty
    bool maybe_null() const { return ptr && !nonnull; }
};

struct State
{
    std::set<int> nn; // the ids of the variables that are known as non null
    bool alive = true;
};

State join(const State& a, const State& b)
{
    if (!a.alive)
        return b;
    if (!b.alive)
        return a;
    State r;
    for (int id : a.nn)
        if (b.nn.count(id))
            r.nn.insert(id);
    return r;
}

const std::string* single_name(const Expr& e)
{
    if (auto* p = std::get_if<ParenExpr>(&e.v))
        return p->inner ? single_name(*p->inner) : nullptr;
    auto* n = std::get_if<NameExpr>(&e.v);
    return n && n->parts.size() == 1 && n->template_args.empty() ? &n->parts[0] : nullptr;
}

bool is_assign_op(const std::string& op)
{
    return op == "=" || (op.size() >= 2 && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=");
}

// The names that a statement list assigns, in a lambda too. A "ref r = p;" counts for both names.
template <class Drive> std::set<std::string> assigned_by(Drive&& drive)
{
    std::set<std::string> names;
    auto fe = [&](const Expr& e)
    {
        if (auto* b = std::get_if<BinaryExpr>(&e.v); b && b->lhs && is_assign_op(b->op))
        {
            if (const std::string* n = single_name(*b->lhs))
                names.insert(*n);
        }
        else if (auto* u = std::get_if<UnaryExpr>(&e.v); u && u->operand && (u->op == "++" || u->op == "--"))
        {
            if (const std::string* n = single_name(*u->operand))
                names.insert(*n);
        }
    };
    auto fs = [&](const Stmt& s)
    {
        if (auto* d = std::get_if<VarDeclStmt>(&s.v); d && d->is_ref && d->init)
            if (const std::string* n = single_name(*d->init))
            {
                names.insert(*n);
                names.insert(d->name);
            }
    };
    auto w = walk::make_walker(fe, fs);
    drive(w);
    return names;
}

struct LoopCtx
{
    bool is_switch = false;
    std::vector<State> breaks;
    std::vector<State> continues;
};

// ------------------------------------------------------------------ the analyzer
class Analyzer
{
public:
    Analyzer(const ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag, const NullOptions& opt)
        : cd_(cd), types_(types), map_(map), diag_(diag), opt_(opt)
    {
    }

    void run()
    {
        for (const Field& f : cd_.fields)
            if (!f.raw)
                init(f.init);
        for (const Method& m : cd_.methods)
            if (!m.raw)
                body(m.params, m.body);
        for (const Ctor& c : cd_.ctors)
            if (!c.raw)
                body(c.params, c.body);
        for (const Method& m : cd_.dtors)
            if (!m.raw)
                body(m.params, m.body);
        for (const Property& p : cd_.props)
        {
            init(p.init);
            if (p.get_body)
                body({}, *p.get_body);
            if (p.set_body)
                body({}, *p.set_body, "value", stype(p));
        }
    }

private:
    struct Var
    {
        Val v;
        bool disabled = false; // a "ref" binds the name: it never narrows
    };

    const ClassDef& cd_;
    const TypeModel& types_;
    const LineMap& map_;
    Diagnostics& diag_;
    const NullOptions& opt_;
    std::vector<Var> vars_;
    std::vector<std::map<std::string, int>> scopes_;
    State st_;
    std::vector<LoopCtx> loops_;
    std::set<std::pair<uint32_t, std::string>> seen_;
    int quiet_ = 0; // not zero: a dry run, nothing is reported
    std::set<std::string>* store_log_ = nullptr; // the names that a call can store through

    // ---------------------------------------------------------------- reports
    void report(uint32_t pos, const std::string& msg, bool always_error = false)
    {
        if (quiet_ > 0)
            return;
        if (!seen_.insert({pos, msg}).second)
            return;
        if (always_error || opt_.as_error)
            diag_.add(pos, map_, msg);
        else
            diag_.warn(pos, map_, msg);
    }

    // The value is accessed: a member, a call, an index, a dereference.
    void require(const Val& v, uint32_t pos)
    {
        if (!v.maybe_null())
            return;
        std::string msg;
        if (v.origin == Origin::Local && !v.text.empty())
            msg = "\"" + v.text + "\" can be null here; check it with \"if (" + v.text + " != null)\" or use \"" + v.text
                + "?.m\"";
        else if (!v.text.empty())
            msg = "\"" + v.text + "\" can be null here; copy it to a local and check it";
        else
            msg = "this value can be null here; copy it to a local and check it";
        report(pos, msg);
    }

    // ---------------------------------------------------------------- scopes and variables
    int find_var(const std::string& name) const
    {
        for (size_t i = scopes_.size(); i-- > 0;)
        {
            auto it = scopes_[i].find(name);
            if (it != scopes_[i].end())
                return it->second;
        }
        return -1;
    }

    int declare(const std::string& name, Val v, bool nonnull)
    {
        v.origin = Origin::Local;
        v.text = name;
        vars_.push_back(Var{v, false});
        const int id = static_cast<int>(vars_.size()) - 1;
        scopes_.back()[name] = id;
        if (nonnull && v.ptr)
            st_.nn.insert(id);
        return id;
    }

    Val var_val(int id) const
    {
        Val v = vars_[static_cast<size_t>(id)].v;
        v.nonnull = !vars_[static_cast<size_t>(id)].disabled && st_.nn.count(id) != 0;
        return v;
    }

    // The variable that a condition can narrow: a plain local name.
    int narrowable(const Expr& e) const
    {
        const std::string* n = single_name(e);
        if (!n)
            return -1;
        const int id = find_var(*n);
        return id >= 0 && !vars_[static_cast<size_t>(id)].disabled ? id : -1;
    }

    void strip_names(State& s, const std::set<std::string>& names) const
    {
        for (const std::string& n : names)
        {
            const int id = find_var(n);
            if (id >= 0)
                s.nn.erase(id);
        }
    }

    // The names that a call in the loop parts can store through ("ref" and "out" arguments). A dry run
    // of the parts finds them, because the callee is known only during the walk. The dry run reports
    // nothing and leaves the state as it was. A loop in a dry run does not start a second dry run.
    template <class Drive> std::set<std::string> stored_by(Drive&& drive)
    {
        std::set<std::string> log;
        if (quiet_ > 0)
            return log;
        const State saved = st_;
        std::vector<LoopCtx> saved_loops;
        saved_loops.swap(loops_);
        std::set<std::string>* saved_log = std::exchange(store_log_, &log);
        ++quiet_;
        loops_.emplace_back();
        drive();
        --quiet_;
        store_log_ = saved_log;
        loops_.swap(saved_loops);
        st_ = saved;
        return log;
    }

    // ---------------------------------------------------------------- types
    static Val from_script(const std::string& type)
    {
        Val v;
        v.type = strip_type(type);
        v.ptr = ptr_like(type);
        v.cnst = type.find("const") != std::string::npos;
        return v;
    }

    // A type that libclang gave for a native value: "T*", a handle or an optional.
    Val from_cpp(const std::string& type) const
    {
        Val v;
        v.type = strip_type(type);
        v.cnst = type.find("const") != std::string::npos;
        if (v.type.empty())
            return v;
        v.ptr = v.type.back() == '*' || v.type.find("optional<") != std::string::npos || types_.is_handle(v.type);
        v.nonnull = v.ptr && types_.returns_nonnull(v.type);
        return v;
    }

    const ClassDef* find_cls(const std::string& type) const
    {
        const std::string n = class_of(type);
        if (n.empty())
            return nullptr;
        if (n == cd_.name)
            return &cd_;
        return opt_.find_class ? opt_.find_class(n) : nullptr;
    }

    const ClassDef* parent_of(const ClassDef& c) const
    {
        if (c.base.empty() || c.base == c.name || !opt_.find_class)
            return nullptr;
        return opt_.find_class(c.base);
    }

    std::optional<Val> member_of(const ClassDef* c, const std::string& name) const
    {
        int depth = 0;
        for (const ClassDef* d = c; d && depth < 64; d = parent_of(*d), ++depth)
        {
            for (const Field& f : d->fields)
                if (f.name == name)
                    return field_val(f);
            for (const Property& p : d->props)
                if (p.name == name)
                    return from_script(stype(p));
        }
        return std::nullopt;
    }

    static Val field_val(const Field& f)
    {
        if (f.value || f.weak)
            return Val{};
        return from_script(stype(f));
    }

    const Method* method_of(const ClassDef* c, const std::string& name) const
    {
        int depth = 0;
        for (const ClassDef* d = c; d && depth < 64; d = parent_of(*d), ++depth)
            for (const Method& m : d->methods)
                if (m.name == name)
                    return &m;
        return nullptr;
    }

    static Val ret_val(const Method& m)
    {
        if (m.ret_value)
            return Val{};
        return from_script(m.script_ret.empty() ? m.ret : m.script_ret);
    }

    // The value of "p?.m" when "m" has the value "m". A pointer or a handle stays nullable. Any other
    // type "T" becomes "T?". A type that is not known, and void, stay unknown.
    static Val nullable_of(Val m)
    {
        if (m.type.empty() || m.type == "void")
            return {};
        if (!m.ptr)
        {
            m.type += "?";
            m.ptr = true;
        }
        m.nonnull = false;
        m.origin = Origin::Other;
        m.text.clear();
        return m;
    }

    // The text of the type in a cast operator "(T)": empty when "u" is not a cast.
    static std::string cast_type(const UnaryExpr& u)
    {
        if (u.postfix || u.op.size() < 3 || u.op.front() != '(' || u.op.back() != ')')
            return {};
        return trim(u.op.substr(1, u.op.size() - 2));
    }

    static std::string no_spaces(const std::string& t)
    {
        std::string r;
        for (char c : t)
            if (c != ' ' && c != '\t')
                r += c;
        return r;
    }

    // True when the cast of "from" to the handle type "to" ("$C") is a cast to the same class or to
    // a script base class. The lowering writes hot::nonnull for it. Any other handle cast can give null.
    bool handle_unwrap(const Val& from, const std::string& to) const
    {
        if (from.type.empty() || from.type[0] != '$' || to[0] != '$')
            return false;
        const std::string target = class_of(to);
        if (class_of(from.type) == target)
            return true;
        int depth = 0;
        for (const ClassDef* d = find_cls(from.type); d && depth < 64; d = parent_of(*d), ++depth)
            if (d->name == target)
                return true;
        return false;
    }

    // The value of the cast "(T)v" of a pointer or a handle. A cast to the same pointer type and a
    // cast of a handle to the same class or a base class is a checked unwrap: it throws on null, so the
    // result is not null. A "const" cast keeps the value. A pointer cast keeps the null state. A handle
    // cast to another class (a downcast) can give null.
    Val cast(const Val& ov, const std::string& ct, const Expr& operand)
    {
        const std::string to = strip_type(ct);
        if (!ov.ptr || to.empty() || !ptr_like(to) || to.back() == '?')
            return {};
        if (starts_with(trim(ct), "const "))
        {
            Val v = ov; // "(const T*)p": a view of the same pointer
            v.type = to;
            v.cnst = true;
            return v;
        }
        Val v = from_script(to);
        v.ptr = true;
        const bool is_handle = to[0] == '$';
        const bool unwrap = is_handle ? handle_unwrap(ov, to) : (to.back() == '*' && !ov.cnst && no_spaces(strip_type(ov.type)) == no_spaces(to));
        if (unwrap)
        {
            v.nonnull = true;
            if (const int id = narrowable(operand); id >= 0)
                st_.nn.insert(id); // the cast threw when the name was null
        }
        else
            v.nonnull = !is_handle && ov.nonnull;
        return v;
    }

    // The member "name" of a value: a field of a script class, else unknown. A field never narrows.
    Val member_val(const Val& obj, const std::string& name) const
    {
        const ClassDef* c = obj.type.empty() ? nullptr : find_cls(obj.type);
        if (!c)
            return {};
        std::optional<Val> m = member_of(c, name);
        if (!m)
            return {};
        m->origin = Origin::Field;
        m->text = obj.text.empty() ? std::string() : obj.text + "." + name;
        return *m;
    }

    // ---------------------------------------------------------------- expressions
    static uint32_t part_pos(const NameExpr& n, size_t i)
    {
        return i < n.part_pos.size() && n.part_pos[i] ? n.part_pos[i] : n.pos;
    }

    // The value of the first "count" parts of a name. Each step is an access of the earlier value.
    Val chain(const NameExpr& n, size_t count)
    {
        if (n.parts.empty() || count == 0)
            return {};
        const std::string& head = n.parts[0];
        Val cur;
        bool known = false;
        if (const int id = find_var(head); id >= 0)
        {
            cur = var_val(id);
            known = true;
        }
        else if (std::optional<Val> f = member_of(&cd_, head))
        {
            cur = *f;
            cur.origin = Origin::Field;
            cur.text = head;
            known = true;
        }
        else if (const Field* mv = opt_.module_var ? opt_.module_var(head) : nullptr)
        {
            cur = field_val(*mv);
            cur.origin = Origin::Field;
            cur.text = head;
            known = true;
        }
        if (!known)
            return {};
        for (size_t i = 1; i < count && i < n.parts.size(); ++i)
        {
            require(cur, part_pos(n, i - 1));
            cur = member_val(cur, n.parts[i]);
        }
        return cur;
    }

    void exprs(const std::vector<ExprP>& list)
    {
        for (const ExprP& a : list)
            if (a)
                expr(*a);
    }

    Val expr(const ExprP& e) { return e ? expr(*e) : Val{}; }

    Val expr(const Expr& e)
    {
        if (auto* n = std::get_if<NameExpr>(&e.v))
        {
            exprs(n->targ_exprs);
            return chain(*n, n->parts.size());
        }
        if (auto* t = std::get_if<ThisExpr>(&e.v))
        {
            (void)t;
            Val v;
            v.type = cd_.name;
            v.text = "this";
            return v;
        }
        if (std::holds_alternative<NullExpr>(e.v))
        {
            Val v;
            v.type = "null";
            v.ptr = true;
            return v;
        }
        if (auto* m = std::get_if<MemberExpr>(&e.v))
        {
            if (!m->obj)
                return {};
            const Val ov = expr(*m->obj);
            if (m->safe)
                return nullable_of(member_val(ov, m->name)); // "p?.m" is the null check
            require(ov, pos_of(*m->obj));
            return member_val(ov, m->name);
        }
        if (auto* c = std::get_if<CallExpr>(&e.v))
            return call(*c);
        if (auto* ix = std::get_if<IndexExpr>(&e.v))
        {
            if (ix->obj)
                require(expr(*ix->obj), pos_of(*ix->obj));
            expr(ix->index);
            return {};
        }
        if (auto* u = std::get_if<UnaryExpr>(&e.v))
            return unary(*u);
        if (auto* b = std::get_if<BinaryExpr>(&e.v))
        {
            if (b->lhs && b->rhs && (b->op == "&&" || b->op == "||"))
            {
                State t, f;
                cond(e, t, f); // the right side sees the narrowing of the left side
                st_ = join(t, f);
                return {};
            }
            return binary(*b);
        }
        if (auto* t = std::get_if<TernaryExpr>(&e.v))
            return ternary(*t);
        if (auto* p = std::get_if<ParenExpr>(&e.v))
            return p->inner ? expr(*p->inner) : Val{};
        if (auto* nw = std::get_if<NewExpr>(&e.v))
        {
            exprs(nw->args);
            // "new $C(...)" is a handle that is not null. "new C(...)" is a value (a box).
            Val v = from_script(nw->type);
            v.nonnull = v.ptr;
            return v;
        }
        if (auto* tt = std::get_if<TypeTestExpr>(&e.v))
        {
            expr(tt->obj);
            if (tt->is_as)
            {
                Val v;
                v.type = tt->type + "*"; // "as" gives a nullable result
                v.ptr = true;
                return v;
            }
            return {};
        }
        if (auto* ip = std::get_if<InterpExpr>(&e.v))
        {
            exprs(ip->args);
            return {};
        }
        if (auto* co = std::get_if<CollectionExpr>(&e.v))
        {
            exprs(co->elems);
            exprs(co->values);
            return {};
        }
        if (auto* l = std::get_if<LambdaExpr>(&e.v))
        {
            lambda(*l);
            return {};
        }
        return {};
    }

    Val unary(const UnaryExpr& u)
    {
        if (!u.operand)
            return {};
        if (!u.postfix && u.op == "&")
        {
            expr(*u.operand);
            Val v;
            v.type = "void*";
            v.ptr = true;
            v.nonnull = true; // the address of an object is not null
            return v;
        }
        const Val ov = expr(*u.operand);
        if (!u.postfix && u.op == "*")
            require(ov, u.pos ? u.pos : pos_of(*u.operand));
        if (const std::string ct = cast_type(u); !ct.empty())
            return cast(ov, ct, *u.operand);
        return {};
    }

    Val binary(const BinaryExpr& b)
    {
        if (!b.lhs || !b.rhs)
        {
            expr(b.lhs);
            expr(b.rhs);
            return {};
        }
        if (b.op == "??")
        {
            const Val l = expr(*b.lhs);
            const State before = st_;
            const Val r = expr(*b.rhs); // the right side runs only when the left side is null
            // A non null left side never runs the right side: the state stays as it is before the right side.
            st_ = l.ptr && l.nonnull ? before : join(before, st_);
            Val v;
            v.type = l.type;
            v.ptr = l.ptr && r.ptr;
            // The result is non null when the right side is non null. A non null left side is the result too.
            v.nonnull = r.nonnull || (l.ptr && l.nonnull);
            if (!r.ptr && !r.type.empty() && l.ptr && !l.type.empty() && l.type.back() == '?')
                v.type = trim(l.type.substr(0, l.type.size() - 1)); // "T? ?? T" is a "T"
            else if (r.nonnull && r.ptr && (l.type.empty() || l.type == "null"))
                v.type = r.type; // the right side is not null: it gives the type
            return v;
        }
        if (is_assign_op(b.op))
        {
            if (b.op == "=")
            {
                if (const std::string* n = single_name(*b.lhs))
                    if (const int id = find_var(*n); id >= 0)
                    {
                        const Val r = expr(*b.rhs);
                        const Var& var = vars_[static_cast<size_t>(id)];
                        if (var.v.ptr && !var.disabled && r.ptr && r.nonnull)
                            st_.nn.insert(id); // a known non null value
                        else
                            st_.nn.erase(id);
                        return {};
                    }
            }
            expr(*b.rhs);
            expr(*b.lhs);
            return {};
        }
        expr(*b.lhs);
        expr(*b.rhs);
        return {};
    }

    Val ternary(const TernaryExpr& t)
    {
        State ts, fs;
        if (t.cond)
            cond(*t.cond, ts, fs);
        else
            ts = fs = st_;
        st_ = ts;
        const Val a = expr(t.a);
        const State sa = st_;
        st_ = fs;
        const Val b = expr(t.b);
        st_ = join(sa, st_);
        Val v;
        v.type = a.type.empty() || a.type == "null" ? b.type : a.type;
        v.ptr = a.ptr && b.ptr;
        v.nonnull = a.nonnull && b.nonnull;
        return v;
    }

    Val call(const CallExpr& c)
    {
        Val result;
        const Method* m = nullptr;
        if (!c.callee)
            return {};
        if (auto* n = std::get_if<NameExpr>(&c.callee->v))
        {
            exprs(n->targ_exprs);
            if (n->parts.size() == 1)
            {
                const std::string& head = n->parts[0];
                if (find_var(head) >= 0 || member_of(&cd_, head) || (opt_.module_var && opt_.module_var(head)))
                    require(chain(*n, 1), n->pos); // a call through a variable
                else if ((m = method_of(&cd_, head)))
                    result = ret_val(*m);
            }
            else
            {
                const Val cur = chain(*n, n->parts.size() - 1);
                require(cur, part_pos(*n, n->parts.size() - 2));
                if (const ClassDef* cls = cur.type.empty() ? nullptr : find_cls(cur.type))
                    if ((m = method_of(cls, n->parts.back())))
                        result = ret_val(*m);
            }
        }
        else if (auto* mm = std::get_if<MemberExpr>(&c.callee->v))
        {
            if (mm->obj)
            {
                const Val ov = expr(*mm->obj);
                if (!mm->safe)
                    require(ov, pos_of(*mm->obj));
                if (const ClassDef* cls = ov.type.empty() ? nullptr : find_cls(ov.type))
                    if ((m = method_of(cls, mm->name)))
                        result = mm->safe ? nullable_of(ret_val(*m)) : ret_val(*m);
            }
        }
        else
        {
            require(expr(*c.callee), pos_of(*c.callee));
        }
        for (size_t i = 0; i < c.args.size(); ++i)
        {
            if (!c.args[i])
                continue;
            expr(*c.args[i]);
            // A "ref" or "out" parameter can store null in the argument. When the callee is not a method of
            // a script class, the modes are not known (a native "T*&", an interface, "base.m(p)"): any
            // name argument can be stored through, so it loses its narrowing.
            const bool stores = !m || (i < m->params.size()
                && (m->params[i].mode == ParamMode::Ref || m->params[i].mode == ParamMode::Out));
            if (stores)
                if (const std::string* n = single_name(*c.args[i]))
                    if (const int id = find_var(*n); id >= 0)
                    {
                        st_.nn.erase(id);
                        if (store_log_)
                            store_log_->insert(*n);
                    }
        }
        return result;
    }

    void lambda(const LambdaExpr& l)
    {
        const State saved = st_;
        std::vector<LoopCtx> saved_loops;
        saved_loops.swap(loops_);
        scopes_.emplace_back();
        for (const Param& p : l.params)
            declare(p.name, p.value || p.weak ? Val{} : from_script(stype(p)), false);
        st_ = State{};
        block(l.body);
        scopes_.pop_back();
        loops_.swap(saved_loops);
        st_ = saved;
        // A captured variable that the lambda assigns loses its narrowing.
        const std::set<std::string> names = assigned_by([&](auto& w) { w.block(l.body); });
        strip_names(st_, names);
    }

    // ---------------------------------------------------------------- conditions
    // Evaluates "e" in the state "st_". "t" is the state when "e" is true, "f" when it is false.
    void cond(const Expr& e, State& t, State& f)
    {
        if (auto* p = std::get_if<ParenExpr>(&e.v); p && p->inner)
        {
            cond(*p->inner, t, f);
            return;
        }
        if (auto* u = std::get_if<UnaryExpr>(&e.v); u && !u->postfix && u->op == "!" && u->operand)
        {
            cond(*u->operand, f, t);
            return;
        }
        if (auto* b = std::get_if<BinaryExpr>(&e.v); b && b->lhs && b->rhs)
        {
            if (b->op == "&&")
            {
                State t1, f1, t2, f2;
                cond(*b->lhs, t1, f1);
                st_ = t1;
                cond(*b->rhs, t2, f2);
                t = t2;
                f = join(f1, f2);
                return;
            }
            if (b->op == "||")
            {
                State t1, f1, t2, f2;
                cond(*b->lhs, t1, f1);
                st_ = f1;
                cond(*b->rhs, t2, f2);
                t = join(t1, t2);
                f = f2;
                return;
            }
            if (b->op == "==" || b->op == "!=")
            {
                const Expr* other = nullptr;
                if (std::holds_alternative<NullExpr>(b->rhs->v))
                    other = b->lhs.get();
                else if (std::holds_alternative<NullExpr>(b->lhs->v))
                    other = b->rhs.get();
                if (other)
                {
                    const int id = narrowable(*other);
                    if (id < 0)
                        expr(*other); // a comparison is not an access of the name
                    State base = st_, with = st_;
                    if (id >= 0)
                        with.nn.insert(id);
                    t = b->op == "!=" ? with : base;
                    f = b->op == "!=" ? base : with;
                    return;
                }
            }
        }
        if (auto* tt = std::get_if<TypeTestExpr>(&e.v); tt && !tt->is_as && tt->obj)
        {
            expr(*tt->obj);
            const int id = narrowable(*tt->obj);
            t = f = st_;
            if (id >= 0)
                t.nn.insert(id);
            if (!tt->bind.empty())
            {
                Val bv;
                bv.type = tt->type + "*";
                bv.ptr = true;
                const int bid = declare(tt->bind, bv, false);
                t.nn.insert(bid); // the name is bound only when the test is true
            }
            return;
        }
        expr(e);
        t = f = st_;
    }

    // ---------------------------------------------------------------- statements
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

    LoopCtx* nearest_loop()
    {
        for (size_t i = loops_.size(); i-- > 0;)
            if (!loops_[i].is_switch)
                return &loops_[i];
        return nullptr;
    }

    // Declares a local from a "var" or a typed declaration.
    void var_decl(const VarDeclStmt& d)
    {
        if (!d.names.empty())
        {
            expr(d.init);
            for (const std::string& n : d.names)
                declare(n, Val{}, false);
            return;
        }
        if (d.is_ref && d.init)
        {
            ref_decl(d);
            return;
        }
        const Val iv = expr(d.init);
        Val v;
        if (d.value || d.weak)
            v = Val{};
        else if (!d.type.empty())
            v = from_script(d.type);
        else
        {
            v = iv;
            if (v.type.empty() && !v.ptr && !d.native_type.empty())
                v = from_cpp(d.native_type);
        }
        v.nonnull = false;
        const bool nn = d.init && v.ptr && (iv.ptr ? iv.nonnull : d.type.empty() && !d.native_type.empty() && from_cpp(d.native_type).nonnull);
        declare(d.name, v, nn);
    }

    // "ref r = x;"
    void ref_decl(const VarDeclStmt& d)
    {
        const Val iv = expr(*d.init);
        if (iv.origin == Origin::Field && iv.ptr)
            report(pos_of(*d.init), "a ref cannot bind to a nullable field", true);
        const int pid = narrowable(*d.init);
        Val v = iv;
        v.nonnull = false;
        if (pid >= 0 && iv.ptr)
        {
            // Both names stay nullable for the rest of the scope.
            vars_[static_cast<size_t>(pid)].disabled = true;
            st_.nn.erase(pid);
            const int rid = declare(d.name, v, false);
            vars_[static_cast<size_t>(rid)].disabled = true;
            return;
        }
        const int rid = declare(d.name, v, iv.ptr && iv.nonnull);
        if (iv.ptr && !iv.nonnull)
            vars_[static_cast<size_t>(rid)].disabled = true;
    }

    void stmt(const Stmt& s)
    {
        if (!st_.alive)
            return; // dead code after a return, a throw, a break or a continue
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
            var_decl(*d);
        }
        else if (auto* i = std::get_if<IfStmt>(&s.v))
        {
            State t, f;
            if (i->cond)
                cond(*i->cond, t, f);
            else
                t = f = st_;
            st_ = t;
            stmt(i->then);
            const State a = st_;
            st_ = f;
            stmt(i->otherwise);
            st_ = join(a, st_);
        }
        else if (auto* w = std::get_if<WhileStmt>(&s.v))
        {
            const auto names = assigned_by(
                [&](auto& wk)
                {
                    wk.expr(w->cond);
                    wk.stmt(w->body);
                });
            strip_names(st_, names);
            strip_names(st_, stored_by(
                [&]
                {
                    expr(w->cond);
                    stmt(w->body);
                }));
            State t, f;
            if (w->cond)
                cond(*w->cond, t, f);
            else
                t = f = st_;
            loops_.emplace_back();
            st_ = t;
            stmt(w->body);
            end_loop(f);
        }
        else if (auto* dw = std::get_if<DoWhileStmt>(&s.v))
        {
            const auto names = assigned_by(
                [&](auto& wk)
                {
                    wk.stmt(dw->body);
                    wk.expr(dw->cond);
                });
            strip_names(st_, names);
            strip_names(st_, stored_by(
                [&]
                {
                    stmt(dw->body);
                    expr(dw->cond);
                }));
            const State entry = st_;
            loops_.emplace_back();
            stmt(dw->body);
            State at_cond = st_;
            for (const State& c : loops_.back().continues)
                at_cond = join(at_cond, c);
            if (!at_cond.alive && loops_.back().continues.empty())
                at_cond = State{{}, false};
            st_ = at_cond.alive ? at_cond : entry;
            State t, f;
            if (dw->cond && at_cond.alive)
                cond(*dw->cond, t, f);
            else
                t = f = st_;
            end_loop(at_cond.alive ? f : State{{}, false});
        }
        else if (auto* fr = std::get_if<ForStmt>(&s.v))
        {
            scopes_.emplace_back();
            for (const StmtP& x : fr->init)
                stmt(x);
            const auto names = assigned_by(
                [&](auto& wk)
                {
                    wk.expr(fr->cond);
                    for (const ExprP& x : fr->step)
                        wk.expr(x);
                    wk.stmt(fr->body);
                });
            strip_names(st_, names);
            strip_names(st_, stored_by(
                [&]
                {
                    expr(fr->cond);
                    for (const ExprP& x : fr->step)
                        expr(x);
                    stmt(fr->body);
                }));
            State t, f;
            if (fr->cond)
                cond(*fr->cond, t, f);
            else
                t = f = st_;
            loops_.emplace_back();
            st_ = t;
            stmt(fr->body);
            for (const State& c : loops_.back().continues)
                st_ = join(st_, c);
            for (const ExprP& x : fr->step)
                expr(x);
            end_loop(f);
            scopes_.pop_back();
        }
        else if (auto* fo = std::get_if<ForOfStmt>(&s.v))
        {
            expr(fo->range);
            scopes_.emplace_back();
            if (fo->names.empty())
                declare(fo->name, fo->value || fo->type.empty() ? Val{} : from_script(fo->type), false);
            else
                for (const std::string& n : fo->names)
                    declare(n, Val{}, false);
            const auto names = assigned_by([&](auto& wk) { wk.stmt(fo->body); });
            strip_names(st_, names);
            strip_names(st_, stored_by([&] { stmt(fo->body); }));
            const State entry = st_;
            loops_.emplace_back();
            stmt(fo->body);
            end_loop(entry);
            scopes_.pop_back();
        }
        else if (auto* r = std::get_if<ReturnStmt>(&s.v))
        {
            expr(r->expr);
            st_.alive = false;
        }
        else if (auto* th = std::get_if<ThrowStmt>(&s.v))
        {
            expr(th->expr);
            st_.alive = false;
        }
        else if (std::holds_alternative<BreakStmt>(s.v))
        {
            if (!loops_.empty())
                loops_.back().breaks.push_back(st_);
            st_.alive = false;
        }
        else if (std::holds_alternative<ContinueStmt>(s.v))
        {
            if (LoopCtx* l = nearest_loop())
                l->continues.push_back(st_);
            st_.alive = false;
        }
        else if (auto* sw = std::get_if<SwitchStmt>(&s.v))
        {
            switch_stmt(*sw);
        }
        else if (auto* tr = std::get_if<TryStmt>(&s.v))
        {
            try_stmt(*tr);
        }
    }

    // The state after a loop: the false path of the condition and the "break" paths.
    void end_loop(State out)
    {
        for (const State& b : loops_.back().breaks)
            out = join(out, b);
        loops_.pop_back();
        st_ = out;
    }

    void switch_stmt(const SwitchStmt& sw)
    {
        expr(sw.subject);
        const State start = st_;
        State exit{{}, false};
        bool has_default = false;
        loops_.emplace_back();
        loops_.back().is_switch = true;
        State prev{{}, false}; // the end of the case before: a fall-through
        for (const SwitchCase& c : sw.cases)
        {
            has_default = has_default || c.is_default;
            st_ = join(start, prev);
            scopes_.emplace_back();
            for (const ExprP& l : c.labels)
                expr(l);
            for (const StmtP& x : c.stmts)
                stmt(x);
            scopes_.pop_back();
            prev = st_;
        }
        exit = prev;
        for (const State& b : loops_.back().breaks)
            exit = join(exit, b);
        loops_.pop_back();
        if (!has_default)
            exit = join(exit, start);
        st_ = exit;
    }

    void try_stmt(const TryStmt& tr)
    {
        const State start = st_;
        std::vector<std::pair<size_t, size_t>> exits; // the "break" and "continue" states that exist before the try
        for (const LoopCtx& l : loops_)
            exits.emplace_back(l.breaks.size(), l.continues.size());
        const auto in_try = assigned_by([&](auto& wk) { wk.block(tr.block); });
        auto all = in_try;
        for (const CatchClause& c : tr.catches)
        {
            const auto n = assigned_by([&](auto& wk) { wk.block(c.block); });
            all.insert(n.begin(), n.end());
        }
        block(tr.block);
        State out = st_;
        for (const CatchClause& c : tr.catches)
        {
            // The narrowing of the try block does not hold: the block can throw at any point.
            st_ = start;
            strip_names(st_, in_try);
            scopes_.emplace_back();
            if (!c.name.empty())
                declare(c.name, Val{}, false);
            block(c.block);
            scopes_.pop_back();
            out = join(out, st_);
        }
        if (tr.finally_block)
        {
            const auto in_fin = assigned_by([&](auto& wk) { wk.block(*tr.finally_block); });
            all.insert(in_fin.begin(), in_fin.end());
            const bool live = out.alive;
            st_ = start;
            strip_names(st_, all);
            block(*tr.finally_block);
            const bool fin_live = st_.alive;
            st_ = out;
            strip_names(st_, in_fin);
            st_.alive = live && fin_live;
            // A "break" or "continue" in the try or in a catch leaves through the finally block. That
            // block can assign a name, so the state that leaves keeps no narrowing of this name.
            for (size_t i = 0; i < exits.size() && i < loops_.size(); ++i)
            {
                for (size_t j = exits[i].first; j < loops_[i].breaks.size(); ++j)
                    strip_names(loops_[i].breaks[j], in_fin);
                for (size_t j = exits[i].second; j < loops_[i].continues.size(); ++j)
                    strip_names(loops_[i].continues[j], in_fin);
            }
        }
        else
            st_ = out;
    }

    // ---------------------------------------------------------------- bodies
    void reset()
    {
        scopes_.clear();
        scopes_.emplace_back();
        st_ = State{};
        loops_.clear();
    }

    void init(const ExprP& e)
    {
        if (!e)
            return;
        reset();
        expr(*e);
    }

    void body(const std::vector<Param>& params, const BlockStmt& b, const char* extra = nullptr,
        const std::string& extra_type = {})
    {
        reset();
        for (const Param& p : params)
            declare(p.name, p.value || p.weak ? Val{} : from_script(stype(p)), false);
        if (extra)
            declare(extra, from_script(extra_type), false);
        block(b);
    }
};

} // namespace

void check_nulls(
    const ClassDef& cd, const TypeModel& types, const LineMap& map, Diagnostics& diag, const NullOptions& opt)
{
    Analyzer(cd, types, map, diag, opt).run();
}

} // namespace hotc
