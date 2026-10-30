// The ".cpp" frontend (see cpp_frontend.h). libclang parses the file. The walk fills an
// ast::Script. The bodies are raw text. The edits change only the references that the
// generated code cannot resolve. An edit never adds a newline, so the line numbers stay.
#include "cpp_frontend.h"

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstring>
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
    std::map<std::string, std::string> module_vars_; // USR -> name
    std::map<std::string, std::string> script_classes_; // USR -> name

    int line(uint32_t pos) const { return map_.line(pos); }

    void collect_bases(CXCursor rec, std::set<std::string>& out);
    void check_type_refs(CXCursor c, const std::string& own_usr);
    void check_module_refs(CXCursor c, bool hook = false);
    void examine(CXCursor k, EditCtx& ctx);
    void walk(CXCursor c, EditCtx& ctx);
    std::string edit_region(CXCursor root, Range region, const ClassInfo* cls);
    void process_class(CXCursor c);
    ast::Field make_field(CXCursor c, const ClassInfo* cls);
    void fill_params(CXCursor c, std::vector<ast::Param>& out, const ClassInfo* cls);
    CXCursor body_of(CXCursor c);
    ast::Method make_method(CXCursor c, const ClassInfo* cls);
    ast::Ctor make_ctor(CXCursor c, const ClassInfo& cls);
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
            auto it = script_classes_.find(u);
            if (it != script_classes_.end() && u != own_usr)
                fail(pos_of(k),
                    "the script class '" + it->second + "' cannot be named outside its body (first version)");
        }
        check_type_refs(k, own_usr);
    }
}

// Fails on a reference to a module variable in a free function or in a convert hook.
void Frontend::check_module_refs(CXCursor c, bool hook)
{
    for (CXCursor k : children(c))
    {
        if (clang_getCursorKind(k) == CXCursor_DeclRefExpr)
        {
            auto it = module_vars_.find(usr(clang_getCursorReferenced(k)));
            if (it != module_vars_.end())
            {
                if (hook) // the same text as the .spp lowering gives
                    fail(pos_of(k),
                        "the module variable '" + it->second +
                            "' cannot be used in a convert hook: the new module state is not complete yet");
                fail(pos_of(k),
                    "a free function cannot use the module variable '" + it->second +
                        "' in a .cpp script (first version)");
            }
        }
        check_module_refs(k, hook);
    }
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
        auto it = module_vars_.find(usr(ref));
        if (it != module_vars_.end())
        {
            add(e.begin, e.end - e.begin, "spp_mod()->" + it->second);
            return;
        }
        if (t.find("::") == std::string::npos && !qualified(e.begin) && base_scoped(ref))
            add(e.begin, 0, ctx.cls->base + "::");
        return;
    }
    case CXCursor_TypeRef:
    {
        CXCursor ref = clang_getCursorReferenced(k);
        auto it = script_classes_.find(usr(ref));
        if (it != script_classes_.end())
            fail(e.begin, "the script class '" + it->second + "' cannot be named outside its body (first version)");
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

// A field or a module variable. The initializer is the text after "=", or "{...}", or "(...)".
ast::Field Frontend::make_field(CXCursor c, const ClassInfo* cls)
{
    ast::Field f;
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
        f.raw_init = trim(edit_region(c, Range{start, decl.end}, cls));
        if (text_[i] == '(')
        { // "T x(a, b);": the text inside the parentheses, so "T(a, b)" is not "T((a, b))"
            if (f.raw_init.size() < 2 || f.raw_init.back() != ')')
                fail(f.pos, "cannot read the initializer of this declaration");
            f.raw_init = trim(f.raw_init.substr(1, f.raw_init.size() - 2));
        }
        f.raw_brace_init = text_[i] != '(' && !f.raw_init.empty() && f.raw_init.front() == '{';
    }
    return f;
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
                p.def->cpp = trim(edit_region(a, extent_of(k), cls));
                p.def->lowered = true;
                p.def->pos = pos_of(k);
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
ast::Method Frontend::make_method(CXCursor c, const ClassInfo* cls)
{
    ast::Method m;
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
    m.raw_body = edit_region(c, Range{b.begin + 1, b.end - 1}, cls);
    return m;
}

ast::Ctor Frontend::make_ctor(CXCursor c, const ClassInfo& cls)
{
    ast::Ctor ct;
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
        ct.raw_base_args = edit_region(c, Range{static_cast<uint32_t>(a + 1), static_cast<uint32_t>(a_end)}, &cls);
    }
    ct.raw_line = line(b.begin); // "raw_body" starts right after the "{"
    ct.raw_body = edit_region(c, Range{b.begin + 1, b.end - 1}, &cls);
    return ct;
}

void Frontend::process_class(CXCursor c)
{
    ast::ClassDef cd;
    cd.name = spelling(c);
    cd.pos = pos_of(c);
    cd.line = line(cd.pos);
    ClassInfo info;
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
            cd.fields.push_back(make_field(k, &info));
            break;
        case CXCursor_VarDecl:
            fail(pos_of(k), "a static data member is not supported in a .cpp script");
        case CXCursor_CXXMethod:
            cd.methods.push_back(make_method(k, &info));
            break;
        case CXCursor_Constructor:
            cd.ctors.push_back(make_ctor(k, info));
            break;
        case CXCursor_Destructor:
            cd.dtors.push_back(make_method(k, &info));
            break;
        default:
            fail(pos_of(k), "this declaration is not supported in a .cpp script class");
        }
    }
    sc_.classes.push_back(std::move(cd));
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

ast::Script Frontend::run(CXTranslationUnit tu, const std::string& name)
{
    main_ = clang_getFile(tu, name.c_str());
    std::vector<CXCursor> top;
    for (CXCursor k : children(clang_getTranslationUnitCursor(tu)))
        if (from_main(k))
            top.push_back(k);
    // The first pass: the names of the script classes and of the module variables.
    for (CXCursor k : top)
    {
        if (is_script_class(k))
            script_classes_[usr(k)] = spelling(k);
        else if (is_module_var(k))
            module_vars_[usr(k)] = spelling(k);
    }
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
        const bool script_class = is_script_class(k);
        check_type_refs(k, script_class ? usr(k) : std::string());
        const Range e = extent_of(k);
        if (script_class)
            process_class(k);
        else if (is_module_var(k))
            sc_.vars.push_back(make_field(k, nullptr));
        else if (kind == CXCursor_FunctionDecl && clang_isCursorDefinition(k))
        {
            const std::string n = spelling(k);
            bool hook = false;
            if (n.rfind("convert_", 0) == 0)
                for (const auto& v : module_vars_)
                    hook = hook || v.second == n.substr(std::strlen("convert_"));
            if (hook)
            {
                check_module_refs(k, true);
                sc_.var_hooks.push_back(make_method(k, nullptr));
            }
            else
            {
                check_module_refs(k);
                add_prelude(k, e);
            }
        }
        else if (kind == CXCursor_MacroDefinition)
        { // the extent starts at the name: take the "#define" of the line too
            size_t b = e.begin;
            while (b > 0 && text_[b - 1] != '\n')
                --b;
            add_prelude(k, Range{static_cast<uint32_t>(b), e.end});
        }
        else
            add_prelude(k, e);
    }
    return std::move(sc_);
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
