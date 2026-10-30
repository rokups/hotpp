// The ".cpp" frontend (see cpp_frontend.h). libclang parses the file. The walk fills an
// ast::Script. The bodies are raw text. The edits change only the references that the
// generated code cannot resolve. An edit never adds a newline, so the line numbers stay.
#include "cpp_frontend.h"

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <utility>

namespace sppc
{
namespace
{

// An error of the frontend. The code throws it only outside the libclang callbacks.
struct CppFail
{
    uint32_t pos;
    std::string msg;
};

[[noreturn]] void fail(uint32_t pos, std::string msg)
{
    throw CppFail{pos, std::move(msg)};
}

const char* const kMacroMsg = "a macro hides a member of the base class; write the member in the script text";

std::string cxs(CXString s)
{
    std::string r = clang_getCString(s) ? clang_getCString(s) : "";
    clang_disposeString(s);
    return r;
}

// A guard for the libclang handles. It releases them on every exit path.
struct Handles
{
    CXIndex idx = nullptr;
    CXTranslationUnit tu = nullptr;
    ~Handles()
    {
        if (tu)
            clang_disposeTranslationUnit(tu);
        if (idx)
            clang_disposeIndex(idx);
    }
};

// Gives the direct children of a cursor. The callback does not throw.
std::vector<CXCursor> children(CXCursor c)
{
    std::vector<CXCursor> out;
    clang_visitChildren(
        c,
        [](CXCursor k, CXCursor, CXClientData d)
        {
            static_cast<std::vector<CXCursor>*>(d)->push_back(k);
            return CXChildVisit_Continue;
        },
        &out);
    return out;
}

uint32_t offset_of(CXSourceLocation l)
{
    CXFile f;
    unsigned line = 0, col = 0, off = 0;
    clang_getExpansionLocation(l, &f, &line, &col, &off);
    return off;
}

struct Range
{
    uint32_t begin = 0, end = 0;
    bool has(uint32_t p) const { return p >= begin && p < end; }
};

Range extent_of(CXCursor c)
{
    CXSourceRange r = clang_getCursorExtent(c);
    return {offset_of(clang_getRangeStart(r)), offset_of(clang_getRangeEnd(r))};
}

uint32_t pos_of(CXCursor c)
{
    return offset_of(clang_getCursorLocation(c));
}

std::string usr(CXCursor c)
{
    return cxs(clang_getCursorUSR(c));
}

std::string spelling(CXCursor c)
{
    return cxs(clang_getCursorSpelling(c));
}

std::string type_text(CXType t)
{
    return cxs(clang_getTypeSpelling(t));
}

bool from_main(CXCursor c)
{
    return clang_Location_isFromMainFile(clang_getCursorLocation(c)) != 0;
}

// True when the spelling location of the cursor is not its expansion location (a macro).
bool in_macro(CXCursor c)
{
    CXSourceLocation l = clang_getCursorLocation(c);
    CXFile ef = nullptr, sf = nullptr;
    unsigned a = 0, b = 0, eo = 0, so = 0;
    clang_getExpansionLocation(l, &ef, &a, &b, &eo);
    clang_getSpellingLocation(l, &sf, &a, &b, &so);
    return !clang_File_isEqual(ef, sf) || eo != so;
}

bool is_record(CXCursorKind k)
{
    return k == CXCursor_ClassDecl || k == CXCursor_StructDecl || k == CXCursor_UnionDecl ||
        k == CXCursor_ClassTemplate || k == CXCursor_ClassTemplatePartialSpecialization;
}

bool has_child(CXCursor c, CXCursorKind kind)
{
    for (CXCursor k : children(c))
        if (clang_getCursorKind(k) == kind)
            return true;
    return false;
}

bool is_ident(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool is_space(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

// True when "word" starts at "i" of "t" and is a whole word.
bool word_at(const std::string& t, size_t i, const char* word)
{
    size_t n = std::strlen(word);
    if (t.compare(i, n, word) != 0)
        return false;
    if (i > 0 && is_ident(t[i - 1]))
        return false;
    return i + n >= t.size() || !is_ident(t[i + n]);
}

size_t skip_ws(const std::string& t, size_t i, size_t limit)
{
    while (i < limit && is_space(t[i]))
        ++i;
    return i;
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b]))
        ++b;
    while (e > b && is_space(s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

// Gives the position after a string literal, a char literal or a comment at "i".
// Gives "i" when none starts there.
size_t skip_opaque(const std::string& t, size_t i)
{
    char q = t[i];
    if (q == '/' && i + 1 < t.size() && t[i + 1] == '/')
    {
        size_t e = t.find('\n', i);
        return e == std::string::npos ? t.size() : e;
    }
    if (q == '/' && i + 1 < t.size() && t[i + 1] == '*')
    {
        size_t e = t.find("*/", i + 2);
        return e == std::string::npos ? t.size() : e + 2;
    }
    if (q != '"' && q != '\'')
        return i;
    if (q == '\'' && i > 0 && std::isalnum(static_cast<unsigned char>(t[i - 1])))
    {
        // A prefix (L, u, U, u8) that is a whole word starts a literal. Other text before the
        // quote is a number, and the quote is a digit separator: 1'000
        size_t b = i;
        while (b > 0 && is_ident(t[b - 1]))
            --b;
        const std::string w = t.substr(b, i - b);
        if (w != "L" && w != "u" && w != "U" && w != "u8")
            return i;
    }
    if (q == '"' && i > 0 && t[i - 1] == 'R')
    { // a raw string: R"d( ... )d"
        size_t open = t.find('(', i);
        if (open == std::string::npos)
            return t.size();
        std::string close = ")" + t.substr(i + 1, open - i - 1) + "\"";
        size_t e = t.find(close, open);
        return e == std::string::npos ? t.size() : e + close.size();
    }
    for (size_t j = i + 1; j < t.size(); ++j)
    {
        if (t[j] == '\\')
            ++j;
        else if (t[j] == q)
            return j + 1;
    }
    return t.size();
}

// Gives the position of the bracket that closes the bracket at "open", before "limit".
// It skips the literals and the comments. Gives npos when the bracket does not close.
size_t match_close(const std::string& t, size_t open, size_t limit)
{
    const char o = t[open];
    const char c = o == '(' ? ')' : o == '{' ? '}' : o == '[' ? ']' : '>';
    int depth = 0;
    for (size_t i = open; i < limit && i < t.size();)
    {
        size_t k = skip_opaque(t, i);
        if (k != i)
        {
            i = k;
            continue;
        }
        if (t[i] == o)
            ++depth;
        else if (t[i] == c && --depth == 0)
            return i;
        ++i;
    }
    return std::string::npos;
}

// The last component of a qualified name, without template arguments: "ns::Base<int>" -> "Base".
std::string last_name(const std::string& s)
{
    std::string r = s.substr(0, s.find('<'));
    size_t c = r.rfind("::");
    return trim(c == std::string::npos ? r : r.substr(c + 2));
}

// The facts of a script class that the body edits need.
struct ClassInfo
{
    std::string name, usr, base;
    std::set<std::string> base_usrs; // the native base and all its ancestors
};

struct Edit
{
    uint32_t pos, len;
    std::string text;
};

// The state of the edits of one region (a body, the base arguments, an initializer).
struct EditCtx
{
    const ClassInfo* cls = nullptr; // null outside a script class (a module variable, a hook)
    Range region;
    std::vector<Edit> edits;
    std::set<uint32_t> this_member; // the offsets of a "this" that a member access uses
    std::vector<Range> this_lambdas; // the lambdas that capture "this" by name
};

class Frontend
{
public:
    Frontend(const std::string& text, const LineMap& map)
        : text_(text)
        , map_(map)
    {
    }

    ast::Script run(CXTranslationUnit tu, const std::string& name);

private:
    CXFile main_ = nullptr;
    const std::string& text_;
    const LineMap& map_;
    ast::Script sc_;

    // The kinds of declaration that the index records.
    enum class Kind
    {
        ScriptClass,
        ScriptField,
        ScriptMethod,
        ScriptStatic,
        ScriptStaticFn,
        ModuleVar
    };

    // One declaration of the script. "mangled" is "<Class>__<name>" for a static member.
    struct Entry
    {
        Kind kind;
        std::string owner_usr, owner_name, name, mangled;
        bool virt = false;
    };

    std::map<std::string, Entry> table_; // USR of the declaration -> entry

    // A text region that needs edits. The functions that read the declarations record a slice.
    // apply_slices() edits all slices in the order of the records. "store" puts the edited text
    // in the model. It finds the target by an index, because the model vectors can move.
    struct Slice
    {
        CXCursor root;
        Range extent;
        const ClassInfo* cls;
        std::function<void(std::string&&)> store;
    };

    std::vector<Slice> slices_;
    std::deque<ClassInfo> infos_; // the class facts that the slices point to

    void add_slice(CXCursor root, Range extent, const ClassInfo* cls, std::function<void(std::string&&)> store);
    void apply_slices();
    void read_top(const std::vector<CXCursor>& top);

    void index(const std::vector<CXCursor>& top);
    void index_member(CXCursor k, const std::string& owner_usr, const std::string& owner_name);
    const Entry* lookup(CXCursor ref) const;

    int line(uint32_t pos) const { return map_.line(pos); }

    void collect_bases(CXCursor rec, std::set<std::string>& out);
    void check_type_refs(CXCursor c, const std::string& own_usr);
    void check_hook_refs(CXCursor c);
    bool find_module_ref(CXCursor c, const CXCursor* skip, std::string& name, uint32_t& pos);
    std::string strip_defaults(CXCursor fn, uint32_t begin, const std::string& sig);
    void add_module_func(CXCursor fn, Range e, const std::string& var, uint32_t var_pos);
    void examine(CXCursor k, EditCtx& ctx);
    void walk(CXCursor c, EditCtx& ctx);
    std::string edit_region(CXCursor root, Range region, const ClassInfo* cls);
    void process_class(CXCursor c);
    template <class T> using Where = std::function<std::vector<T>&()>; // gives the vector of a model list

    void make_field(CXCursor c, const ClassInfo* cls, const Where<ast::Field>& where);
    void fill_params(CXCursor c, std::vector<ast::Param>& out, const ClassInfo* cls);
    CXCursor body_of(CXCursor c);
    void make_method(CXCursor c, const ClassInfo* cls, const Where<ast::Method>& where);
    void make_ctor(CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where);
    void add_prelude(CXCursor c, Range e);
    void add_import(CXCursor c);
};

// Adds the USRs of the bases of the record "rec" and of their ancestors.
void Frontend::collect_bases(CXCursor rec, std::set<std::string>& out)
{
    for (CXCursor k : children(rec))
    {
        if (clang_getCursorKind(k) != CXCursor_CXXBaseSpecifier)
            continue;
        CXCursor decl = clang_getTypeDeclaration(clang_getCursorType(k));
        if (clang_Cursor_isNull(decl))
            continue;
        CXCursor def = clang_getCursorDefinition(decl);
        if (!clang_Cursor_isNull(def))
            decl = def;
        if (out.insert(usr(decl)).second)
            collect_bases(decl, out);
    }
}

// Fails on a type reference to a script class other than "own_usr".
void Frontend::check_type_refs(CXCursor c, const std::string& own_usr)
{
    for (CXCursor k : children(c))
    {
        if (clang_getCursorKind(k) == CXCursor_TypeRef)
        {
            std::string u = usr(clang_getCursorReferenced(k));
            const Entry* en = lookup(clang_getCursorReferenced(k));
            if (en && en->kind == Kind::ScriptClass && u != own_usr)
                fail(pos_of(k),
                    "the script class '" + en->name + "' cannot be named outside its body (first version)");
        }
        check_type_refs(k, own_usr);
    }
}

// Fails on a reference to a module variable in a convert hook.
void Frontend::check_hook_refs(CXCursor c)
{
    std::string name;
    uint32_t pos = 0;
    if (find_module_ref(c, nullptr, name, pos)) // the same text as the .spp lowering gives
        fail(pos,
            "the module variable '" + name +
                "' cannot be used in a convert hook: the new module state is not complete yet");
}

// Finds the first reference to a module variable below "c". It does not search below "skip".
bool Frontend::find_module_ref(CXCursor c, const CXCursor* skip, std::string& name, uint32_t& pos)
{
    for (CXCursor k : children(c))
    {
        if (skip && clang_equalCursors(k, *skip))
            continue;
        if (clang_getCursorKind(k) == CXCursor_DeclRefExpr)
        {
            const Entry* en = lookup(clang_getCursorReferenced(k));
            if (en && en->kind == Kind::ModuleVar)
            {
                name = en->name;
                pos = pos_of(k);
                return true;
            }
        }
        if (find_module_ref(k, skip, name, pos))
            return true;
    }
    return false;
}

// Gives "sig", the text of a function head from "begin", without the default arguments. A
// definition must not repeat the defaults of the prototype. The new lines stay, so the lines
// of the script map to the same lines.
std::string Frontend::strip_defaults(CXCursor fn, uint32_t begin, const std::string& sig)
{
    std::vector<std::pair<size_t, size_t>> cuts; // [from, to) in "sig"
    for (CXCursor a : children(fn))
    {
        if (clang_getCursorKind(a) != CXCursor_ParmDecl)
            continue;
        const Range pr = extent_of(a);
        int depth = 0;
        for (size_t i = pr.begin; i < pr.end && i < begin + sig.size();)
        {
            const size_t k = skip_opaque(text_, i);
            if (k != i)
            {
                i = k;
                continue;
            }
            const char ch = text_[i];
            if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if (ch == ')' || ch == ']' || ch == '}')
                --depth;
            else if (ch == '=' && depth == 0 && i + 1 < text_.size() && text_[i + 1] != '=' &&
                std::strchr("=!<>", text_[i - 1]) == nullptr)
            {
                size_t from = i;
                while (from > pr.begin && is_space(text_[from - 1]))
                    --from;
                cuts.emplace_back(from - begin, std::min<size_t>(pr.end - begin, sig.size()));
                break;
            }
            ++i;
        }
    }
    std::string out = sig;
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it)
    {
        std::string keep;
        for (size_t i = it->first; i < it->second; ++i)
            if (sig[i] == '\n')
                keep += '\n';
        out.replace(it->first, it->second - it->first, keep);
    }
    return out;
}

// A free function that uses a module variable. The body calls spp_mod(), and the module block
// comes after the prelude. So the prelude gets the prototype and "post_funcs" gets the
// definition with the edited body.
void Frontend::add_module_func(CXCursor fn, Range e, const std::string& var, uint32_t var_pos)
{
    const CXCursor body = body_of(fn);
    std::string name;
    uint32_t pos = 0;
    if (find_module_ref(fn, &body, name, pos))
        fail(pos, "a default argument cannot use the module variable '" + name + "' in a .cpp script");
    const Range b = extent_of(body);
    const std::string sig = text_.substr(e.begin, b.begin - e.begin);
    for (size_t i = 0; i < sig.size(); ++i)
        if (word_at(sig, i, "constexpr") || word_at(sig, i, "consteval"))
            fail(var_pos,
                "a constexpr function cannot use the module variable '" + var +
                    "' in a .cpp script: sppc moves the body after the module block");
    ast::Script::RawDecl proto;
    proto.pos = e.begin;
    proto.line = line(e.begin);
    proto.text = trim(sig) + ";";
    sc_.prelude.push_back(std::move(proto));
    ast::Script::RawDecl def;
    def.pos = e.begin;
    def.line = line(e.begin);
    def.text = strip_defaults(fn, e.begin, sig);
    const size_t index = sc_.post_funcs.size();
    const std::string head = def.text;
    sc_.post_funcs.push_back(std::move(def));
    add_slice(fn, Range{b.begin, b.end}, nullptr,
        [this, index, head](std::string&& s) { sc_.post_funcs[index].text = head + s; });
}

// Records the edit of one cursor of a region. The cases of the switch are the rules.
void Frontend::examine(CXCursor k, EditCtx& ctx)
{
    // A cursor of a macro expansion is not "from the main file" for libclang. The expansion
    // location gives its place in the script.
    CXFile ef = nullptr;
    unsigned l = 0, col = 0, off = 0;
    clang_getExpansionLocation(clang_getCursorLocation(k), &ef, &l, &col, &off);
    if (!clang_File_isEqual(ef, main_))
        return;
    const CXCursorKind kind = clang_getCursorKind(k);
    const Range e = extent_of(k);
    if (!ctx.region.has(e.begin))
        return;
    const bool macro = in_macro(k);
    auto add = [&](uint32_t pos, uint32_t len, std::string rep)
    {
        if (macro)
            fail(e.begin, kMacroMsg);
        ctx.edits.push_back(Edit{pos, len, std::move(rep)});
    };
    // True when the text before "pos" ends with "::" (a qualified name).
    auto qualified = [&](uint32_t pos)
    {
        size_t i = pos;
        while (i > 0 && is_space(text_[i - 1]))
            --i;
        return i >= 2 && text_.compare(i - 2, 2, "::") == 0;
    };
    // True when "ref" is a static member or a nested type of the native base or its ancestors.
    auto base_scoped = [&](CXCursor ref)
    {
        if (!ctx.cls)
            return false;
        CXCursor p = clang_getCursorSemanticParent(ref);
        if (clang_getCursorKind(p) == CXCursor_EnumDecl && !clang_EnumDecl_isScoped(p))
            p = clang_getCursorSemanticParent(p);
        return is_record(clang_getCursorKind(p)) && ctx.cls->base_usrs.count(usr(p)) != 0;
    };
    const std::string t = e.end > e.begin ? text_.substr(e.begin, e.end - e.begin) : std::string();

    switch (kind)
    {
    case CXCursor_MemberRefExpr:
    {
        if (!ctx.cls)
            return;
        CXCursor ref = clang_getCursorReferenced(k);
        if (clang_Cursor_isNull(ref))
            return;
        const std::string parent = usr(clang_getCursorSemanticParent(ref));
        const bool need = ctx.cls->base_usrs.count(parent) != 0 ||
            (parent == ctx.cls->usr && clang_getCursorKind(ref) == CXCursor_CXXMethod &&
                has_child(ref, CXCursor_CXXOverrideAttr));
        if (need && macro)
            fail(e.begin, kMacroMsg);
        // The object: an explicit "this ->", an implicit "this", or another object.
        bool explicit_this = false;
        size_t p = 0;
        if (word_at(t, 0, "this"))
        {
            size_t q = skip_ws(t, 4, t.size());
            if (t.compare(q, 2, "->") == 0)
            {
                explicit_this = true;
                p = skip_ws(t, q + 2, t.size());
            }
        }
        const std::string rest = t.substr(p);
        if (rest.find('.') != std::string::npos || rest.find("->") != std::string::npos)
            return; // another object, for example "other.Move"
        if (explicit_this)
            ctx.this_member.insert(e.begin);
        if (!need)
            return;
        if (explicit_this)
            add(e.begin, 4, "__self");
        else
            add(e.begin, 0, "__self->");
        return;
    }
    case CXCursor_CXXThisExpr:
    {
        if (!ctx.cls || ctx.this_member.count(e.begin) || !word_at(text_, e.begin, "this"))
            return; // a member access uses it, or the "this" is implicit
        // The "this" of a local class in the body is not the script object.
        if (usr(clang_getTypeDeclaration(clang_getPointeeType(clang_getCursorType(k)))) != ctx.cls->usr)
            return;
        add(e.begin, 4, "__self");
        return;
    }
    case CXCursor_DeclRefExpr:
    {
        CXCursor ref = clang_getCursorReferenced(k);
        if (clang_Cursor_isNull(ref))
            return;
        const Entry* en = lookup(ref);
        if (en && en->kind == Kind::ModuleVar)
        {
            add(e.begin, e.end - e.begin, "spp_mod()->" + en->name);
            return;
        }
        if (t.find("::") == std::string::npos && !qualified(e.begin) && base_scoped(ref))
            add(e.begin, 0, ctx.cls->base + "::");
        return;
    }
    case CXCursor_TypeRef:
    {
        CXCursor ref = clang_getCursorReferenced(k);
        const Entry* en = lookup(ref);
        if (en && en->kind == Kind::ScriptClass)
            fail(e.begin, "the script class '" + en->name + "' cannot be named outside its body (first version)");
        if (!qualified(e.begin) && base_scoped(ref))
            add(e.begin, 0, ctx.cls->base + "::");
        return;
    }
    case CXCursor_LambdaExpr:
    {
        size_t close = t.find(']');
        std::string capture = t.substr(0, close == std::string::npos ? t.size() : close);
        for (size_t i = 0; i < capture.size(); ++i)
            if (word_at(capture, i, "this"))
            {
                ctx.this_lambdas.push_back(e);
                break;
            }
        return;
    }
    default:
        return;
    }
}

void Frontend::walk(CXCursor c, EditCtx& ctx)
{
    for (CXCursor k : children(c))
    {
        examine(k, ctx); // the parent first: a member access marks its "this"
        walk(k, ctx);
    }
}

// Gives the text of "region" with the edits of the cursors below "root".
std::string Frontend::edit_region(CXCursor root, Range region, const ClassInfo* cls)
{
    EditCtx ctx;
    ctx.cls = cls;
    ctx.region = region;
    walk(root, ctx);
    for (const Range& l : ctx.this_lambdas)
        for (const Edit& ed : ctx.edits)
            if (l.has(ed.pos))
                fail(l.begin, "a lambda that uses a base member must capture with [&] or [=]");
    std::stable_sort(ctx.edits.begin(), ctx.edits.end(), [](const Edit& a, const Edit& b) { return a.pos > b.pos; });
    std::string s = text_.substr(region.begin, region.end - region.begin);
    const Edit* prev = nullptr;
    for (const Edit& ed : ctx.edits)
    {
        if (prev && prev->pos == ed.pos && prev->len == ed.len && prev->text == ed.text)
            continue; // the same edit twice
        s.replace(ed.pos - region.begin, ed.len, ed.text);
        prev = &ed;
    }
    return s;
}

void Frontend::add_slice(CXCursor root, Range extent, const ClassInfo* cls, std::function<void(std::string&&)> store)
{
    slices_.push_back(Slice{root, extent, cls, std::move(store)});
}

// Edits the slices in the order of the records. So the first error is the same as with an edit
// at the time of the record.
void Frontend::apply_slices()
{
    for (size_t i = 0; i < slices_.size(); ++i)
    {
        const Slice& sl = slices_[i];
        sl.store(edit_region(sl.root, sl.extent, sl.cls));
    }
    slices_.clear();
}

// A field or a module variable. The initializer is the text after "=", or "{...}", or "(...)".
// The function adds the field to the list that "where" gives.
void Frontend::make_field(CXCursor c, const ClassInfo* cls, const Where<ast::Field>& where)
{
    std::vector<ast::Field>& out = where();
    out.emplace_back();
    const size_t index = out.size() - 1;
    ast::Field& f = out.back();
    f.raw = true;
    f.name = spelling(c);
    f.type = f.script_type = type_text(clang_getCursorType(c));
    f.pos = pos_of(c);
    f.line = line(f.pos);
    switch (clang_getCXXAccessSpecifier(c))
    {
    case CX_CXXPrivate:
        f.access = ast::Access::Private;
        break;
    case CX_CXXProtected:
        f.access = ast::Access::Protected;
        break;
    default:
        break;
    }
    for (CXCursor k : children(c))
        if (clang_getCursorKind(k) == CXCursor_AnnotateAttr)
        {
            std::string a = spelling(k);
            if (a.rfind("spp.alias=", 0) == 0)
                f.alias = a.substr(std::strlen("spp.alias="));
        }
    // The text finds the initializer. libclang does not visit every initializer: it skips
    // the "{...}" of a class-type field. The edits of such an initializer are then missing.
    const Range decl = extent_of(c);
    size_t i = f.pos + f.name.size();
    for (;;)
    {
        i = skip_ws(text_, i, decl.end);
        if (i < decl.end && text_[i] == '[')
        { // an array bound or an attribute "[[...]]"
            size_t e = match_close(text_, i, decl.end);
            i = e == std::string::npos ? decl.end : e + 1;
            continue;
        }
        if (i < decl.end && is_ident(text_[i]))
        { // a macro attribute: SPP_ALIAS("old")
            size_t n = i;
            while (n < decl.end && is_ident(text_[n]))
                ++n;
            n = skip_ws(text_, n, decl.end);
            if (n < decl.end && text_[n] == '(')
            {
                size_t e = match_close(text_, n, decl.end);
                i = e == std::string::npos ? decl.end : e + 1;
                continue;
            }
        }
        break;
    }
    if (i < decl.end && (text_[i] == '=' || text_[i] == '{' || text_[i] == '('))
    {
        const uint32_t start = static_cast<uint32_t>(text_[i] == '=' ? i + 1 : i);
        f.has_init = true;
        const char open = text_[i];
        const uint32_t fpos = f.pos;
        add_slice(c, Range{start, decl.end}, cls, [where, index, open, fpos](std::string&& s) {
            ast::Field& fd = where()[index];
            fd.raw_init = trim(s);
            if (open == '(')
            { // "T x(a, b);": the text inside the parentheses, so "T(a, b)" is not "T((a, b))"
                if (fd.raw_init.size() < 2 || fd.raw_init.back() != ')')
                    fail(fpos, "cannot read the initializer of this declaration");
                fd.raw_init = trim(fd.raw_init.substr(1, fd.raw_init.size() - 2));
            }
            fd.raw_brace_init = open != '(' && !fd.raw_init.empty() && fd.raw_init.front() == '{';
        });
    }
}

void Frontend::fill_params(CXCursor c, std::vector<ast::Param>& out, const ClassInfo* cls)
{
    const int n = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < n; ++i)
    {
        CXCursor a = clang_Cursor_getArgument(c, static_cast<unsigned>(i));
        ast::Param p;
        p.name = spelling(a);
        p.type = type_text(clang_getCursorType(a));
        for (CXCursor k : children(a))
            if (clang_isExpression(clang_getCursorKind(k)))
            {
                // The text is already C++. The edits are the same as in a body. The shared
                // default is "lowered", so the .spp path ("default_arg") writes the text as is.
                p.def = std::make_shared<ast::ParamDefault>();
                p.def->lowered = true;
                p.def->pos = pos_of(k);
                std::shared_ptr<ast::ParamDefault> def = p.def;
                add_slice(a, extent_of(k), cls, [def](std::string&& s) { def->cpp = trim(s); });
                break;
            }
        out.push_back(std::move(p));
    }
}

CXCursor Frontend::body_of(CXCursor c)
{
    for (CXCursor k : children(c))
        if (clang_getCursorKind(k) == CXCursor_CompoundStmt)
            return k;
    fail(pos_of(c), "define the method inside the class");
}

ast::Access access_of(CXCursor c)
{
    switch (clang_getCXXAccessSpecifier(c))
    {
    case CX_CXXPrivate:
        return ast::Access::Private;
    case CX_CXXProtected:
        return ast::Access::Protected;
    default:
        return ast::Access::Public;
    }
}

// A method, a destructor or a convert hook. "cls" is null for a hook.
void Frontend::make_method(CXCursor c, const ClassInfo* cls, const Where<ast::Method>& where)
{
    std::vector<ast::Method>& out = where();
    out.emplace_back();
    const size_t index = out.size() - 1;
    ast::Method& m = out.back();
    m.raw = true;
    m.name = spelling(c);
    m.pos = pos_of(c);
    m.line = line(m.pos);
    m.access = access_of(c);
    const CXCursorKind kind = clang_getCursorKind(c);
    if (kind == CXCursor_CXXMethod)
    {
        if (clang_CXXMethod_isStatic(c))
            fail(m.pos, "a static method is not supported in a .cpp script");
        m.is_override = has_child(c, CXCursor_CXXOverrideAttr);
        if (clang_CXXMethod_isVirtual(c) && !m.is_override)
            fail(m.pos, "'virtual' is not supported in a .cpp script; use 'override' of a native virtual");
    }
    if (kind != CXCursor_Destructor)
        m.ret = type_text(clang_getResultType(clang_getCursorType(c)));
    fill_params(c, m.params, cls);
    if (m.name == "onReload")
        for (ast::Param& p : m.params)
        {
            std::string t;
            for (char ch : p.type)
                if (!is_space(ch))
                    t += ch;
            if (t == "spp::OldState" || t == "constspp::OldState&" || t == "OldState" || t == "constOldState&")
                p.type = "OldState";
        }
    const Range b = extent_of(body_of(c));
    m.end_pos = b.end - 1;
    m.end_line = line(m.end_pos);
    m.raw_line = line(b.begin); // "raw_body" starts right after the "{"
    add_slice(c, Range{b.begin + 1, b.end - 1}, cls,
        [where, index](std::string&& s) { where()[index].raw_body = std::move(s); });
}

void Frontend::make_ctor(CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where)
{
    std::vector<ast::Ctor>& out = where();
    out.emplace_back();
    const size_t index = out.size() - 1;
    ast::Ctor& ct = out.back();
    ct.raw = true;
    ct.pos = pos_of(c);
    ct.line = line(ct.pos);
    ct.access = access_of(c);
    fill_params(c, ct.params, &cls);
    const Range b = extent_of(body_of(c));
    ct.end_pos = b.end - 1;
    ct.end_line = line(ct.end_pos);
    // The text between the ")" of the parameters and the "{" of the body has the initializers.
    size_t open = text_.find('(', ct.pos);
    size_t close = open == std::string::npos ? open : match_close(text_, open, b.begin);
    if (close == std::string::npos)
        fail(ct.pos, "cannot find the parameter list of the constructor");
    size_t i = skip_ws(text_, close + 1, b.begin);
    // Skip the words before the ":" (for example "noexcept").
    while (i < b.begin && text_[i] != ':')
        ++i;
    if (i < b.begin)
    {
        const uint32_t colon = static_cast<uint32_t>(i);
        size_t s = skip_ws(text_, i + 1, b.begin);
        size_t n = s;
        while (n < b.begin && (is_ident(text_[n]) || text_[n] == ':' || text_[n] == '<' || text_[n] == '>'))
            ++n;
        const std::string word = text_.substr(s, n - s);
        size_t a = skip_ws(text_, n, b.begin);
        size_t a_end = a < b.begin && (text_[a] == '(' || text_[a] == '{') ? match_close(text_, a, b.begin)
                                                                           : std::string::npos;
        if (word.empty() || last_name(word) != last_name(cls.base) || a_end == std::string::npos ||
            skip_ws(text_, a_end + 1, b.begin) != b.begin)
            fail(colon, "use a default member initializer");
        ct.has_base_call = true;
        ct.base_pos = static_cast<uint32_t>(s);
        add_slice(c, Range{static_cast<uint32_t>(a + 1), static_cast<uint32_t>(a_end)}, &cls,
            [where, index](std::string&& s) { where()[index].raw_base_args = std::move(s); });
    }
    ct.raw_line = line(b.begin); // "raw_body" starts right after the "{"
    add_slice(c, Range{b.begin + 1, b.end - 1}, &cls,
        [where, index](std::string&& s) { where()[index].raw_body = std::move(s); });
}

void Frontend::process_class(CXCursor c)
{
    const size_t ci = sc_.classes.size(); // the slices find the class by this index
    sc_.classes.emplace_back();
    ast::ClassDef& cd = sc_.classes.back(); // valid until the next class
    cd.name = spelling(c);
    cd.pos = pos_of(c);
    cd.line = line(cd.pos);
    infos_.emplace_back();
    ClassInfo& info = infos_.back();
    info.name = cd.name;
    info.usr = usr(c);
    for (CXCursor k : children(c))
    {
        if (clang_getCursorKind(k) != CXCursor_CXXBaseSpecifier)
            continue;
        CXCursor decl = clang_getTypeDeclaration(clang_getCursorType(k));
        if (!clang_Cursor_isNull(decl) && from_main(decl))
            fail(pos_of(k), "a .cpp script class cannot derive from a class of the same file (first version)");
        std::string b = type_text(clang_getCursorType(k));
        if (cd.base.empty())
            cd.base = b;
        else
            cd.interfaces.push_back(b);
    }
    info.base = cd.base;
    collect_bases(c, info.base_usrs);
    for (CXCursor k : children(c))
    {
        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_CXXBaseSpecifier || kind == CXCursor_CXXAccessSpecifier || clang_isAttribute(kind))
            continue;
        switch (kind)
        {
        case CXCursor_FieldDecl:
            make_field(k, &info, [this, ci]() -> std::vector<ast::Field>& { return sc_.classes[ci].fields; });
            break;
        case CXCursor_VarDecl:
            fail(pos_of(k), "a static data member is not supported in a .cpp script");
        case CXCursor_CXXMethod:
            make_method(k, &info, [this, ci]() -> std::vector<ast::Method>& { return sc_.classes[ci].methods; });
            break;
        case CXCursor_Constructor:
            make_ctor(k, info, [this, ci]() -> std::vector<ast::Ctor>& { return sc_.classes[ci].ctors; });
            break;
        case CXCursor_Destructor:
            make_method(k, &info, [this, ci]() -> std::vector<ast::Method>& { return sc_.classes[ci].dtors; });
            break;
        default:
            fail(pos_of(k), "this declaration is not supported in a .cpp script class");
        }
    }
}

// A verbatim file-scope declaration. A ";" after the extent belongs to it.
void Frontend::add_prelude(CXCursor c, Range e)
{
    ast::Script::RawDecl d;
    d.pos = e.begin;
    d.line = line(e.begin);
    d.text = text_.substr(e.begin, e.end - e.begin);
    size_t i = skip_ws(text_, e.end, text_.size());
    if (i < text_.size() && text_[i] == ';')
        d.text += ';';
    (void)c;
    sc_.prelude.push_back(std::move(d));
}

// An "#include" directive gives an import. The quote or the angle bracket after "include"
// gives the form. The cursor spelling is the name without the delimiters.
void Frontend::add_import(CXCursor c)
{
    const Range e = extent_of(c);
    size_t i = text_.find("include", e.begin);
    i = i == std::string::npos ? text_.size() : skip_ws(text_, i + std::strlen("include"), text_.size());
    ast::Import im;
    im.path = i < text_.size() && text_[i] == '<' ? "<" + spelling(c) + ">" : spelling(c);
    im.pos = e.begin;
    im.line = line(im.pos);
    sc_.imports.push_back(std::move(im));
}

bool is_module_var(CXCursor c)
{
    return clang_getCursorKind(c) == CXCursor_VarDecl && clang_isCursorDefinition(c) &&
        !clang_isConstQualifiedType(clang_getCursorType(c)) && clang_Cursor_getStorageClass(c) != CX_SC_Extern;
}

bool is_script_class(CXCursor c)
{
    const CXCursorKind k = clang_getCursorKind(c);
    return (k == CXCursor_ClassDecl || k == CXCursor_StructDecl) && clang_isCursorDefinition(c) &&
        has_child(c, CXCursor_CXXBaseSpecifier);
}

// Records a member of the script class "owner_usr". A static member is recorded, but
// process_class still rejects it. An out-of-class method definition has the USR of its
// declaration: the first entry stays.
void Frontend::index_member(CXCursor k, const std::string& owner_usr, const std::string& owner_name)
{
    const CXCursorKind kind = clang_getCursorKind(k);
    Entry en{Kind::ScriptField, owner_usr, owner_name, spelling(k), std::string()};
    if (kind == CXCursor_VarDecl)
    {
        en.kind = Kind::ScriptStatic;
        en.mangled = owner_name + "__" + en.name;
    }
    else if (kind == CXCursor_CXXMethod)
    {
        const bool is_static = clang_CXXMethod_isStatic(k) != 0;
        en.kind = is_static ? Kind::ScriptStaticFn : Kind::ScriptMethod;
        if (is_static)
            en.mangled = owner_name + "__" + en.name;
        en.virt = clang_CXXMethod_isVirtual(k) || has_child(k, CXCursor_CXXOverrideAttr);
    }
    else if (kind != CXCursor_FieldDecl)
        return;
    table_.emplace(usr(k), std::move(en));
}

// Index the declarations of the main file: the script classes with their members and the module
// variables. The second loop is for out-of-class method definitions. It adds nothing yet.
void Frontend::index(const std::vector<CXCursor>& top)
{
    for (CXCursor k : top)
    {
        if (is_script_class(k))
        {
            const std::string u = usr(k), n = spelling(k);
            table_[u] = Entry{Kind::ScriptClass, u, n, n, std::string()};
            for (CXCursor m : children(k))
                index_member(m, u, n);
        }
        else if (is_module_var(k))
            table_[usr(k)] = Entry{Kind::ModuleVar, std::string(), std::string(), spelling(k), std::string()};
    }
    for (CXCursor k : top)
    {
        if (clang_getCursorKind(k) != CXCursor_CXXMethod)
            continue;
        auto it = table_.find(usr(clang_getCursorSemanticParent(k)));
        if (it != table_.end() && it->second.kind == Kind::ScriptClass)
            index_member(k, it->first, it->second.name);
    }
}

const Frontend::Entry* Frontend::lookup(CXCursor ref) const
{
    auto it = table_.find(usr(ref));
    return it == table_.end() ? nullptr : &it->second;
}

ast::Script Frontend::run(CXTranslationUnit tu, const std::string& name)
{
    main_ = clang_getFile(tu, name.c_str());
    std::vector<CXCursor> top;
    for (CXCursor k : children(clang_getTranslationUnitCursor(tu)))
        if (from_main(k))
            top.push_back(k);
    index(top);
    try
    {
        read_top(top);
    }
    catch (const CppFail&)
    { // an earlier slice with an error is the first error: the edit was at the record before
        apply_slices();
        throw;
    }
    apply_slices();
    return std::move(sc_);
}

// Reads the declarations of the main file in the order of the text. It records the slices.
void Frontend::read_top(const std::vector<CXCursor>& top)
{
    for (CXCursor k : top)
    {
        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_InclusionDirective)
        { // only a real directive: not one in a comment or in a skipped "#if 0" region
            add_import(k);
            continue;
        }
        if (kind == CXCursor_MacroExpansion)
            continue;
        // A declaration that is not a definition (a prototype) has the USR of its definition.
        const Entry* en = clang_isCursorDefinition(k) ? lookup(k) : nullptr;
        const bool script_class = en && en->kind == Kind::ScriptClass;
        check_type_refs(k, script_class ? usr(k) : std::string());
        const Range e = extent_of(k);
        if (script_class)
            process_class(k);
        else if (en && en->kind == Kind::ModuleVar)
            make_field(k, nullptr, [this]() -> std::vector<ast::Field>& { return sc_.vars; });
        else if (kind == CXCursor_FunctionDecl && clang_isCursorDefinition(k))
        {
            const std::string n = spelling(k);
            bool hook = false;
            if (n.rfind("convert_", 0) == 0)
                for (const auto& v : table_)
                    hook = hook ||
                        (v.second.kind == Kind::ModuleVar && v.second.name == n.substr(std::strlen("convert_")));
            std::string var;
            uint32_t var_pos = 0;
            if (hook)
            {
                check_hook_refs(k);
                make_method(k, nullptr, [this]() -> std::vector<ast::Method>& { return sc_.var_hooks; });
            }
            else if (find_module_ref(k, nullptr, var, var_pos))
                add_module_func(k, e, var, var_pos);
            else
                add_prelude(k, e);
        }
        else if (kind == CXCursor_MacroDefinition)
        { // the extent starts at the name: take the "#define" of the line too
            size_t b = e.begin;
            while (b > 0 && text_[b - 1] != '\n')
                --b;
            add_prelude(k, Range{static_cast<uint32_t>(b), e.end});
        }
        else
        {
            std::string var;
            uint32_t var_pos = 0;
            if (kind == CXCursor_FunctionTemplate && find_module_ref(k, nullptr, var, var_pos))
                fail(var_pos,
                    "a function template cannot use the module variable '" + var +
                        "' in a .cpp script: sppc moves the body after the module block");
            if (kind == CXCursor_Namespace && find_module_ref(k, nullptr, var, var_pos))
                fail(var_pos,
                    "a function in a namespace cannot use the module variable '" + var +
                        "' in a .cpp script: sppc moves the body after the module block");
            add_prelude(k, e);
        }
    }
}

} // namespace

ast::Script parse_cpp_script(const std::string& text, const std::string& name,
    const std::vector<std::string>& clang_args, const LineMap& map, Diagnostics& diag)
{
    ast::Script sc;
    std::vector<const char*> cargs;
    for (const auto& a : clang_args)
        cargs.push_back(a.c_str());
    Handles h;
    h.idx = clang_createIndex(0, 0);
    CXUnsavedFile unsaved{name.c_str(), text.c_str(), static_cast<unsigned long>(text.size())};
    CXErrorCode ec = clang_parseTranslationUnit2(h.idx, name.c_str(), cargs.data(), static_cast<int>(cargs.size()),
        &unsaved, 1, CXTranslationUnit_DetailedPreprocessingRecord, &h.tu);
    if (ec != CXError_Success || !h.tu)
    {
        diag.add_line(0, "libclang cannot parse the script");
        return sc;
    }
    bool diag_error = false;
    for (unsigned i = 0, n = clang_getNumDiagnostics(h.tu); i < n; ++i)
    {
        CXDiagnostic d = clang_getDiagnostic(h.tu, i);
        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error)
        {
            CXSourceLocation loc = clang_getDiagnosticLocation(d);
            if (clang_Location_isFromMainFile(loc))
                diag.add(offset_of(loc), map, cxs(clang_getDiagnosticSpelling(d)));
            else
            { // an error in a header: use the presumed location, as for the imports
                CXString file;
                unsigned line = 0, col = 0;
                clang_getPresumedLocation(loc, &file, &line, &col);
                const char* sev = clang_getDiagnosticSeverity(d) == CXDiagnostic_Fatal ? "fatal error" : "error";
                diag.add_foreign(cxs(file), static_cast<int>(line), static_cast<int>(col), sev,
                    cxs(clang_getDiagnosticSpelling(d)));
            }
            diag_error = true;
        }
        clang_disposeDiagnostic(d);
    }
    if (diag_error)
        return sc;
    try
    {
        Frontend fe(text, map);
        sc = fe.run(h.tu, name);
    }
    catch (const CppFail& f)
    {
        diag.add(f.pos, map, f.msg);
        return ast::Script{};
    }
    sc.is_cpp = true;
    return sc;
}

} // namespace sppc
