// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The ".cpp" frontend (see cpp_frontend.h). libclang parses the file. The walk fills an
// ast::Script. The bodies are raw text. The edits change only the references that the
// generated code cannot resolve. An edit never adds a newline, so the line numbers stay.
#include "cpp_frontend.h"
#include "../file_time.h"
#include "../operator_names.h"

#include "../std_ns.h"

#include <clang-c/Index.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace hotc
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

// The calls of a shell macro in the body of a script class (see Frontend::find_shell_macros). The
// members that such a call declares are not script members, so "children" hides them.
struct HiddenCalls
{
    CXFile file = nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // the offsets [begin, end) in the main file
};
HiddenCalls* g_hidden = nullptr;

bool is_hidden(CXCursor k)
{
    CXFile f = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getExpansionLocation(clang_getCursorLocation(k), &f, &line, &col, &off);
    if (!f || !clang_File_isEqual(f, g_hidden->file))
        return false;
    for (const auto& r : g_hidden->ranges)
        if (off >= r.first && off < r.second)
            return true;
    return false;
}

// Gives the direct children of a cursor. The callback does not throw.
std::vector<CXCursor> children(CXCursor c)
{
    std::vector<CXCursor> out;
    clang_visitChildren(
        c,
        [](CXCursor k, CXCursor, CXClientData d)
        {
            if (!g_hidden || !is_hidden(k))
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
    return unexpand_std(cxs(clang_getTypeSpelling(t))); // the placeholder of the namespace (std_ns.h)
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

// The name of a construct that a script class does not support, for the error message.
// The text "this declaration" is for a construct with no own name.
std::string unsupported_member(CXCursorKind kind)
{
    switch (kind)
    {
    case CXCursor_StructDecl:
    case CXCursor_ClassDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl:
        return "a nested type";
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
        return "a type alias";
    case CXCursor_FriendDecl:
        return "a friend declaration";
    case CXCursor_ConversionFunction:
        return "a conversion operator";
    case CXCursor_FunctionTemplate:
    case CXCursor_ClassTemplate:
    case CXCursor_TypeAliasTemplateDecl:
        return "a member template";
    case CXCursor_StaticAssert:
        return "a static_assert";
    case CXCursor_UsingDeclaration:
        return "a using declaration";
    default:
        return "this declaration";
    }
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

// The namespace path of a declaration: "game::ai". It is empty at file scope. A class between
// the declaration and the namespace does not count.
std::string ns_of(CXCursor c)
{
    std::vector<std::string> parts;
    for (CXCursor p = clang_getCursorSemanticParent(c);
         !clang_Cursor_isNull(p) && clang_getCursorKind(p) != CXCursor_TranslationUnit;
         p = clang_getCursorSemanticParent(p))
        if (clang_getCursorKind(p) == CXCursor_Namespace)
            parts.push_back(spelling(p));
    std::string r;
    for (size_t i = parts.size(); i-- > 0;)
        r += (r.empty() ? "" : "::") + parts[i];
    return r;
}

// The file-scope name that hotc generates for a name in a namespace: "game::ai", "Brain" gives
// "game__ai__Brain". The name stays the same at file scope.
std::string mangle_in(const std::string& ns, const std::string& name)
{
    std::string r;
    for (size_t i = 0; i < ns.size(); ++i)
    {
        if (ns.compare(i, 2, "::") == 0)
        {
            r += "__";
            ++i;
        }
        else
            r += ns[i];
    }
    return ns.empty() ? name : r + "__" + name;
}

// The facts of a script class that the body edits need.
struct ClassInfo
{
    std::string name, usr; // "name" is the file-scope name: "game__Player" for "game::Player"
    std::string simple, display; // the name as written ("Player") and the qualified name ("game::Player")
    std::string base_simple; // the name of the base as written, without the namespace
    std::string base; // the direct base: a native class, or the script parent of a child class
    std::string native; // the native base of the root script class of the chain
    uint32_t end = 0; // the end of the class in the text
    std::set<std::string> base_usrs; // the native base and all its ancestors
    std::set<std::string> script_bases; // the USRs of the script parent and of its script ancestors
};

struct Edit
{
    uint32_t pos, len;
    std::string text;
    bool keep = false; // a second equal edit at the same place is a real edit: do not merge it
};

// The state of the one walk of the main file (see apply_slices). All offsets are absolute.
struct WalkState
{
    const ClassInfo* cls = nullptr; // the enclosing script class; null outside one (a module variable, a hook)
    size_t slice = 0; // the slice that holds the cursor that examine looks at
    std::set<uint32_t> this_member; // the offsets of a "this" that a member access uses
    uint32_t skip_end = 0; // the cursors that start before this offset are inside an expression that has its edit
    // The lambdas with the default capture "&" and no item "this", with the offset of the "]". A lambda
    // that stays can use the local "__self" only by copy, so apply_slices adds the capture "__self".
    std::vector<std::pair<Range, uint32_t>> ref_lambdas;
    std::vector<Range> lambdas; // the lambdas of the walk so far. A cursor inside one of them is in a lambda.
    std::vector<std::string> lambda_owner; // the class of each lambda, for the stale call log ("" outside a class)
    // The start of each lambda that an assignment puts into a field or a module variable.
    std::set<uint32_t> stored_lambdas;
    // The locals of an enclosing function that each lambda uses (start of the lambda, name, place of the use).
    std::vector<std::tuple<uint32_t, std::string, uint32_t>> outer_locals;
    std::vector<std::optional<CppFail>> fails; // the first error of the walk in each slice
};

class Frontend
{
public:
    Frontend(const std::string& text, const LineMap& map, bool aot_optimized)
        : text_(text)
        , map_(map)
        , aot_optimized_(aot_optimized)
    {
    }

    ast::Script run(CXTranslationUnit tu, const std::string& name);

    // The warnings of the read: the byte offset in the text and the message.
    const std::vector<std::pair<uint32_t, std::string>>& warnings() const { return warnings_; }

private:
    std::vector<std::pair<uint32_t, std::string>> warnings_;
    CXFile main_ = nullptr;
    const std::string& text_;
    const LineMap& map_;
    bool aot_optimized_ = false; // the fields are members of the shell: no state_of
    ast::Script sc_;
    template <class T> using Where = std::function<std::vector<T>&()>; // gives the vector of a model list

    // The kinds of declaration that the index records.
    enum class Kind
    {
        ScriptClass,
        ScriptField,
        ScriptMethod,
        ScriptStatic, // a static data member: a module variable
        ScriptStaticConst, // a static const or constexpr data member: a const at file scope
        ScriptStaticFn,
        ModuleVar
    };

    // One declaration of the script. "mangled" is "<Class>__s_<name>" for a static member.
    // "cpos" is the start of the class of a member.
    // "vowner" is the script class with the "virtual" declaration of a script virtual method. It is
    // empty for an override of a native virtual and for a method that is not virtual.
    struct Entry
    {
        Kind kind;
        std::string owner_usr, owner_name, name, mangled;
        bool virt = false;
        uint32_t cpos = 0;
        std::string vowner;
        // The names for a message and for the class record. They differ from "name" and
        // "owner_name" for a declaration in a namespace: "game::Player" and "game__Player".
        std::string display, owner_display;
        const std::string& shown() const { return display.empty() ? name : display; }
        const std::string& owner_shown() const { return owner_display.empty() ? owner_name : owner_display; }
    };

    // The output of a static method: a prototype before the class block and the definition after
    // it. The two header and body slices give the text.
    // A static const member uses only "proto".
    struct StaticFn
    {
        size_t proto = 0, def = 0; // the indexes of the chunks
    };
    std::map<std::string, StaticFn> static_fns_; // USR of the static method -> its chunks
    std::map<std::string, size_t> static_vars_; // USR of a static data member -> the index in the module variables

    std::map<std::string, Entry> table_; // USR of the declaration -> entry

    // A method, a constructor or a destructor that the class declares without a body. The
    // definition comes later in the file, outside the class. The definition gives the body.
    // "method" or "ctor" finds the model entry by "index".
    struct Declared
    {
        Where<ast::Method> method;
        Where<ast::Ctor> ctor;
        size_t index = 0;
        const ClassInfo* cls = nullptr;
        uint32_t pos = 0;
        std::string name, what; // "Player::hit" and "method", "constructor" or "destructor"
        bool defined = false;
    };
    std::map<std::string, Declared> declared_; // USR of the declaration -> its record
    std::set<std::string> pure_usrs_; // USR of each pure virtual method ("= 0")
    std::map<std::string, std::set<std::string>> open_pure_; // class USR -> pure methods with no override

    // A text region that needs edits. The functions that read the declarations record a slice.
    // apply_slices() edits all slices in the order of the records. "store" puts the edited text
    // in the model. It finds the target by an index, because the model vectors can move. "cls"
    // is the class that the walk must find for the slice. The walk checks it.
    struct Slice
    {
        Range extent;
        const ClassInfo* cls;
        std::function<void(std::string&&)> store;
        std::string field; // the field of a default member initializer of a script class, else empty
    };

    // The shell macros of the script classes. A class gets them from the annotations of its header
    // (HOT_SHELL_MACRO and HOT_NO_SLOT in hot/script.h). The key is the position of the class.
    struct ShellInfo
    {
        std::string prelude;   // the calls of the shell macros, as written in the class
        std::string overrides; // the names of the virtuals that get no slot
    };
    std::map<uint32_t, ShellInfo> shell_info_;
    HiddenCalls hidden_;
    void find_shell_macros();
    std::vector<CXCursor> top_; // the top-level cursors of the main file. A namespace with script code is flattened.
    std::string cur_ns_; // the namespace path of the declaration that read_top reads
    std::vector<Slice> slices_;
    std::vector<size_t> by_pos_; // the indexes of the slices in the order of the extents
    std::vector<Edit> edits_; // the edits of the walk, with absolute offsets
    std::vector<Edit> fixed_edits_; // the edits that the readers record (the names of static members)
    std::deque<ClassInfo> infos_; // the class facts that the slices point to
    std::map<std::string, const ClassInfo*> class_infos_; // USR of a script class -> its facts
    // The first module variable. The module block is at its place in the output.
    uint32_t first_var_pos_ = UINT32_MAX;
    std::string first_var_name_;

    void add_slice(Range extent, const ClassInfo* cls, std::function<void(std::string&&)> store);
    bool slice_at(uint32_t pos, size_t& out) const;
    void apply_slices();
    void read_top(const std::vector<CXCursor>& top);

    void index(const std::vector<CXCursor>& top);
    void check_static_names(CXCursor cls, const std::string& cls_name, const std::string& display);
    void index_member(CXCursor k, const std::string& owner_usr, const std::string& owner_name,
        const std::string& owner_display, uint32_t cpos);
    void add_top(CXCursor k);
    bool holds_script(CXCursor ns) const;
    const Entry* lookup(CXCursor ref) const;

    int line(uint32_t pos) const { return map_.line(pos); }

    void collect_bases(CXCursor rec, std::set<std::string>& out);
    std::string slot_owner(CXCursor m) const;
    const Entry* script_base_of(CXCursor rec) const;
    const Entry* class_entry(CXType t) const;
    std::string type_in_script(CXType t) const;
    void check_unique_name(CXCursor k, const Entry& en);
    void collect_ns_types(CXType t, std::vector<std::pair<std::string, std::string>>& out, int depth) const;
    bool names_script_class(CXCursor c) const;
    void check_class_use(CXCursor k, CXCursor parent, const Entry& en, bool static_parent);
    void check_type_refs(CXCursor c);
    void check_hook_refs(CXCursor c);
    // Which references find_module_ref looks for.
    enum : unsigned
    {
        kVars = 1, // a module variable or a static data member
        kFns = 2, // a static method
        kConsts = 4, // a static const data member
        kAll = 7
    };
    bool find_module_ref(CXCursor c, const CXCursor* skip, std::string& name, uint32_t& pos, unsigned what = kVars);
    bool is_static_ref(CXCursor c) const;
    bool scope_follows(CXCursor k) const;
    bool qualifies_static(CXCursor parent) const;
    bool no_side_effects(CXCursor c) const;
    bool static_hook_of(CXCursor m, const std::string& owner_usr, std::string* var) const;
    void pre_class(CXCursor c);
    void drop_static_word(CXCursor c);
    void make_static_const(CXCursor c, const ClassInfo* cls);
    void make_static_fn(CXCursor c, const ClassInfo* cls);
    void read_static_def(CXCursor c);
    void check_late_refs(CXCursor var);
    void check_static_init(CXCursor c);
    void warn_static_locals(CXCursor fn);
    void warn_plain_text(CXCursor k);
    void examine(CXCursor k, WalkState& st);
    void visit(CXCursor k, WalkState& st);
    void walk_main(WalkState& st);
    void process_class(CXCursor c);
    void make_field(CXCursor c, const ClassInfo* cls, const Where<ast::Field>& where, const std::string& mangled = {});
    void read_init(CXCursor c, const ClassInfo* cls, const Where<ast::Field>& where, size_t index);
    bool in_array_bound(Range r) const;
    void fill_params(CXCursor c, std::vector<ast::Param>& out, const ClassInfo* cls);
    CXCursor body_of(CXCursor c);
    bool has_body(CXCursor c);
    bool is_defaulted(CXCursor c);
    bool is_member_def(CXCursor c) const;
    void read_method_body(CXCursor c, const ClassInfo* cls, const Where<ast::Method>& where, size_t index);
    void read_ctor_body(CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where, size_t index);
    void read_member_inits(
        CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where, size_t index, uint32_t colon, Range b);
    void declare(CXCursor c, const ClassInfo* cls, size_t index, const Where<ast::Method>& m, const Where<ast::Ctor>& ct);
    void define_member(CXCursor c);
    void check_late_decls(CXCursor c, const Declared& d, uint32_t from);
    void check_all_defined();
    void make_method(CXCursor c, const ClassInfo* cls, const Where<ast::Method>& where);
    void make_ctor(CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where);
    void add_text(Range e, bool edit);
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

// The name of the script class that declares the slot of the virtual method "m": the class with
// the first "virtual" declaration. An override of a native virtual and a method that is not
// virtual give an empty string. The index records the classes in the order of the text, so the
// table already holds the classes of the chain.
std::string Frontend::slot_owner(CXCursor m) const
{
    if (clang_getCursorKind(m) != CXCursor_CXXMethod || !clang_CXXMethod_isVirtual(m))
        return {};
    CXCursor* over = nullptr;
    unsigned n = 0;
    clang_getOverriddenCursors(m, &over, &n);
    std::string owner;
    for (unsigned i = 0; i < n && owner.empty(); ++i)
        owner = slot_owner(over[i]);
    const bool root = n == 0;
    clang_disposeOverriddenCursors(over);
    if (!root)
        return owner;
    auto it = table_.find(usr(clang_getCursorSemanticParent(m)));
    return it != table_.end() && it->second.kind == Kind::ScriptClass ? it->second.name : std::string();
}

// The entry of the script class that is a base of the record "rec", or null.
const Frontend::Entry* Frontend::script_base_of(CXCursor rec) const
{
    for (CXCursor k : children(rec))
    {
        if (clang_getCursorKind(k) != CXCursor_CXXBaseSpecifier)
            continue;
        const Entry* en = class_entry(clang_getCursorType(k));
        if (en)
            return en;
    }
    return nullptr;
}

// The entry of the script class that the type "t" names, or null. A pointer or a reference is
// not the class.
const Frontend::Entry* Frontend::class_entry(CXType t) const
{
    CXCursor d = clang_getTypeDeclaration(clang_getCanonicalType(t));
    if (clang_Cursor_isNull(d) || clang_getCursorKind(d) == CXCursor_NoDeclFound)
        return nullptr;
    const Entry* en = lookup(d);
    return en && en->kind == Kind::ScriptClass ? en : nullptr;
}

// Collects the script classes of a namespace that a type names, with the spelling in the text.
void Frontend::collect_ns_types(CXType t, std::vector<std::pair<std::string, std::string>>& out, int depth) const
{
    if (depth > 8)
        return;
    switch (t.kind)
    {
    case CXType_Pointer:
        collect_ns_types(clang_getPointeeType(t), out, depth + 1);
        return;
    case CXType_LValueReference:
    case CXType_RValueReference:
        collect_ns_types(clang_getPointeeType(t), out, depth + 1);
        return;
    case CXType_ConstantArray:
    case CXType_IncompleteArray:
    case CXType_VariableArray:
        collect_ns_types(clang_getArrayElementType(t), out, depth + 1);
        return;
    default:
        break;
    }
    if (const Entry* en = class_entry(t))
    {
        if (en->display != en->name)
        {
            std::string text = type_text(t);
            for (const char* q : {"const ", "volatile "})
                if (text.compare(0, std::strlen(q), q) == 0)
                    text.erase(0, std::strlen(q));
            out.emplace_back(text, en->name);
        }
        return;
    }
    CXType named = t.kind == CXType_Elaborated ? clang_Type_getNamedType(t) : t;
    const int n = clang_Type_getNumTemplateArguments(named);
    for (int i = 0; i < n; ++i)
    {
        CXType a = clang_Type_getTemplateArgumentAsType(named, static_cast<unsigned>(i));
        if (a.kind != CXType_Invalid)
            collect_ns_types(a, out, depth + 1);
    }
}

// The text of a type as the generated code needs it: the name of a script class in a namespace
// becomes the file scope name of the class.
std::string Frontend::type_in_script(CXType t) const
{
    std::string text = type_text(t);
    std::vector<std::pair<std::string, std::string>> names;
    collect_ns_types(t, names, 0);
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [from, to] : names)
    {
        size_t at = 0;
        while ((at = text.find(from, at)) != std::string::npos)
        {
            const size_t end = at + from.size();
            const bool left = at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_' || text[at - 1] == ':');
            const bool right = end >= text.size() || !(std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_');
            if (left && right)
            {
                text.replace(at, from.size(), to);
                at += to.size();
            }
            else
                at = end;
        }
    }
    return text;
}

// True when a declaration below "c" names a script class, or uses a field or a method of one.
// The walk must then edit the text of the declaration.
bool Frontend::names_script_class(CXCursor c) const
{
    for (CXCursor k : children(c))
    {
        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_TypeRef || kind == CXCursor_MemberRefExpr)
        {
            const Entry* en = lookup(clang_getCursorReferenced(k));
            if (en && (en->kind == Kind::ScriptClass || en->kind == Kind::ScriptField || en->kind == Kind::ScriptMethod))
                return true;
        }
        if (names_script_class(k))
            return true;
    }
    return false;
}

// Checks one use "k" of the name of the script class "en". A .cpp script uses a script class
// through a pointer or a reference. "sizeof" and "alignof" are also valid. A name before "::" is
// valid only for a static member. All other uses fail: a value, an array, a template argument.
void Frontend::check_class_use(CXCursor k, CXCursor parent, const Entry& en, bool static_parent)
{
    const std::string cls = "the script class '" + en.shown() + "'";
    size_t i = skip_ws(text_, extent_of(k).end, text_.size());
    for (bool more = true; more;)
    {
        more = false;
        for (const char* cv : {"const", "volatile"})
            if (word_at(text_, i, cv))
            {
                i = skip_ws(text_, i + std::strlen(cv), text_.size());
                more = true;
            }
    }
    const char next = i < text_.size() ? text_[i] : '\0';
    if (next == '*' || next == '&')
        return;
    if (text_.compare(i, 2, "::") == 0)
    {
        if (static_parent)
            return;
        fail(pos_of(k), cls + " can qualify only a static member in a .cpp script");
    }
    if (clang_getCursorKind(parent) == CXCursor_UnaryExpr)
    {
        const uint32_t b = extent_of(parent).begin;
        if (word_at(text_, b, "sizeof") || word_at(text_, b, "alignof"))
            return;
    }
    size_t p = extent_of(k).begin;
    while (p > 0 && is_space(text_[p - 1]))
        --p;
    const char prev = p > 0 ? text_[p - 1] : '\0';
    std::string form = cls + " by value";
    const CXTypeKind pk = clang_getCursorType(parent).kind;
    if (next == '[' || pk == CXType_ConstantArray || pk == CXType_IncompleteArray || pk == CXType_VariableArray ||
        pk == CXType_DependentSizedArray)
        form = "an array of " + cls; // "S a[2]": the declaration has the array type
    else if (next == '>' || (next == ',' && prev == '<'))
        form = cls + " as a template argument";
    fail(pos_of(k), form + " is not supported in a .cpp script; use a pointer or a reference");
}

// Fails on a use of a script class name that a .cpp script does not support (see
// check_class_use), on "typeid" of a script class and on a placement or array "new" of one.
void Frontend::check_type_refs(CXCursor c)
{
    const CXCursorKind ck = clang_getCursorKind(c);
    // Only a class at file scope or in a namespace is a script class. A local class or a nested
    // class has no shell, so it cannot derive from a script class.
    if ((ck == CXCursor_ClassDecl || ck == CXCursor_StructDecl) && clang_isCursorDefinition(c))
    {
        const Entry* self = lookup(c);
        if (!self || self->kind != Kind::ScriptClass)
            for (CXCursor k : children(c))
                if (clang_getCursorKind(k) == CXCursor_CXXBaseSpecifier)
                    if (const Entry* b = class_entry(clang_getCursorType(k)))
                        fail(pos_of(k),
                            "the class '" + spelling(c) + "' cannot derive from the script class '" + b->shown() +
                                "': only a class at file scope can derive from a script class in a .cpp script");
    }
    // A script class has no C++ RTTI that works after a reload: each generation has its own
    // type info. So "typeid" of a script class fails. dynamic_cast uses the class records.
    if (ck == CXCursor_CXXTypeidExpr)
        for (CXCursor k : children(c))
        {
            const Entry* en = nullptr;
            if (clang_getCursorKind(k) == CXCursor_TypeRef)
            {
                en = lookup(clang_getCursorReferenced(k));
                en = en && en->kind == Kind::ScriptClass ? en : nullptr;
            }
            else
            {
                CXType t = clang_getCursorType(k);
                for (int n = 0; n < 8 && (t.kind == CXType_Pointer || t.kind == CXType_LValueReference ||
                                             t.kind == CXType_RValueReference);
                     ++n)
                    t = clang_getPointeeType(t);
                en = class_entry(t);
            }
            if (en)
                fail(pos_of(c),
                    "'typeid' of the script class '" + en->shown() +
                        "' is not supported in a .cpp script: a script class has no C++ RTTI; use dynamic_cast");
        }
    // "new C(args)": the TypeRef of C is valid. The placement and the array forms fail.
    const Entry* alloc = nullptr;
    if (ck == CXCursor_CXXNewExpr)
    {
        alloc = class_entry(clang_getPointeeType(clang_getCursorType(c)));
        if (alloc)
            for (CXCursor k : children(c))
            {
                const CXCursorKind kind = clang_getCursorKind(k);
                if (kind == CXCursor_TypeRef && lookup(clang_getCursorReferenced(k)) == alloc)
                {
                    if (skip_ws(text_, extent_of(k).end, text_.size()) < text_.size() &&
                        text_[skip_ws(text_, extent_of(k).end, text_.size())] == '[')
                        fail(pos_of(c),
                            "an array 'new' of the script class '" + alloc->shown() +
                                "' is not supported in a .cpp script; use an array of pointers");
                    break;
                }
                if (clang_isExpression(kind))
                    fail(pos_of(c),
                        "a placement 'new' of the script class '" + alloc->shown() +
                            "' is not supported in a .cpp script");
            }
    }
    // The class name in "Player::count" is the qualifier of a static member: it is allowed.
    // The class name in "Player::m(a)" qualifies a member of a parent class. The body edits check
    // that the class is a script parent and that the object is "this". Only a name before the
    // member name is a qualifier: a template argument comes after it, as in "p->Get<Kid*>()".
    const bool static_parent = qualifies_static(c);
    const uint32_t member_at = ck == CXCursor_MemberRefExpr ? pos_of(c) : 0;
    for (CXCursor k : children(c))
    {
        // A base specifier of a script class is an error of process_class.
        if (clang_getCursorKind(k) == CXCursor_TypeRef && ck != CXCursor_CXXBaseSpecifier)
        {
            const Entry* en = lookup(clang_getCursorReferenced(k));
            // The name of the class in "Player::Player" and "Player::~Player" is not a use.
            // A constructor can name its own class in a delegating initializer. read_ctor_body gives the error.
            const bool own_name = (ck == CXCursor_Constructor || (ck == CXCursor_Destructor && is_member_def(c))) &&
                usr(clang_getCursorReferenced(k)) == usr(clang_getCursorSemanticParent(c));
            // The initializer "Player(args)" of the script parent in the constructor of a child.
            const bool parent_init = ck == CXCursor_Constructor && en &&
                script_base_of(clang_getCursorSemanticParent(c)) == en;
            if (en && en->kind == Kind::ScriptClass && en != alloc && !own_name && !parent_init)
                check_class_use(k, c, *en, static_parent || pos_of(k) < member_at);
        }
        check_type_refs(k);
    }
}

// Fails on a reference to a module variable in a convert hook.
void Frontend::check_hook_refs(CXCursor c)
{
    std::string name;
    uint32_t pos = 0;
    if (find_module_ref(c, nullptr, name, pos)) // the same text as the .hot lowering gives
        fail(pos,
            "the module variable '" + name +
                "' cannot be used in a convert hook: the new module state is not complete yet");
    if (find_module_ref(c, nullptr, name, pos, kFns))
        fail(pos, "the static method '" + name + "' cannot be used in a convert hook");
}

// True when the text after the cursor "k" starts with "::" (the cursor is a qualifier).
bool Frontend::scope_follows(CXCursor k) const
{
    const size_t i = skip_ws(text_, extent_of(k).end, text_.size());
    return text_.compare(i, 2, "::") == 0;
}

// True when "parent" is a use of a static member, or the definition of one outside its class. It
// is also true for a method, a constructor or a destructor defined outside its class. The class
// name can be a qualifier below it.
bool Frontend::qualifies_static(CXCursor parent) const
{
    if (is_static_ref(parent) || is_member_def(parent))
        return true;
    if (clang_getCursorKind(parent) != CXCursor_VarDecl)
        return false;
    const Entry* en = lookup(parent);
    return en && (en->kind == Kind::ScriptStatic || en->kind == Kind::ScriptStaticConst);
}

// True when "c" is a use of a static member of a script class.
bool Frontend::is_static_ref(CXCursor c) const
{
    const CXCursorKind kind = clang_getCursorKind(c);
    if (kind != CXCursor_DeclRefExpr && kind != CXCursor_MemberRefExpr)
        return false;
    const Entry* en = lookup(clang_getCursorReferenced(c));
    return en && (en->kind == Kind::ScriptStatic || en->kind == Kind::ScriptStaticConst ||
                     en->kind == Kind::ScriptStaticFn);
}

// Finds the first reference to a module variable below "c". It does not search below "skip".
// "what" selects the kinds of reference. A static data member is a module variable. The name of
// a static member is "<Class>::<name>".
bool Frontend::find_module_ref(CXCursor c, const CXCursor* skip, std::string& name, uint32_t& pos, unsigned what)
{
    for (CXCursor k : children(c))
    {
        if (skip && clang_equalCursors(k, *skip))
            continue;
        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr)
        {
            const Entry* en = lookup(clang_getCursorReferenced(k));
            unsigned have = 0;
            if (en)
                switch (en->kind)
                {
                case Kind::ModuleVar:
                case Kind::ScriptStatic:
                    have = kVars;
                    break;
                case Kind::ScriptStaticFn:
                    have = kFns;
                    break;
                case Kind::ScriptStaticConst:
                    have = kConsts;
                    break;
                default:
                    break;
                }
            if (have & what)
            {
                name = en->owner_name.empty() ? en->shown() : en->owner_shown() + "::" + en->name;
                pos = pos_of(k);
                return true;
            }
        }
        if (find_module_ref(k, skip, name, pos, what))
            return true;
    }
    return false;
}

// True when the expression "c" has no side effect, so the code can drop it: a name, "this", a
// member access, a dereference.
bool Frontend::no_side_effects(CXCursor c) const
{
    switch (clang_getCursorKind(c))
    {
    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr:
    case CXCursor_CXXThisExpr:
    case CXCursor_ParenExpr:
    case CXCursor_UnexposedExpr:
    case CXCursor_TypeRef:
    case CXCursor_NamespaceRef:
    case CXCursor_TemplateRef:
        break;
    case CXCursor_UnaryOperator:
    {
        const uint32_t b = extent_of(c).begin;
        if (b >= text_.size() || text_[b] != '*')
            return false;
        break;
    }
    default:
        return false;
    }
    for (CXCursor k : children(c))
        if (!no_side_effects(k))
            return false;
    return true;
}

// The module block is at the place of the first module variable. It holds the fields and the
// initializers of all module variables. So a module variable can use only the declarations of
// the main file that come before the first module variable. A type needs its definition there.
// A pointer or a reference needs only a declaration of the type. A template argument needs the
// definition, because hotc cannot know if the template needs the complete type.
void Frontend::check_late_refs(CXCursor var)
{
    const Range own = extent_of(var);
    // The type below all pointers, references and arrays of the variable type, when there is
    // a pointer or a reference: for example "Fwd" in "Fwd* p[2]".
    CXCursor pointee = clang_getNullCursor();
    {
        CXType t = clang_getCursorType(var);
        bool indirect = false;
        for (int i = 0; i < 64; ++i)
        {
            if (t.kind == CXType_ConstantArray || t.kind == CXType_IncompleteArray)
                t = clang_getArrayElementType(t);
            else if (t.kind == CXType_Pointer || t.kind == CXType_LValueReference || t.kind == CXType_RValueReference)
            {
                t = clang_getPointeeType(t);
                indirect = true;
            }
            else
                break;
        }
        if (indirect)
            pointee = clang_getCanonicalCursor(clang_getTypeDeclaration(clang_getCanonicalType(t)));
    }
    std::function<void(CXCursor)> walk = [&](CXCursor c)
    {
        for (CXCursor k : children(c))
        {
            const CXCursorKind kind = clang_getCursorKind(k);
            const bool type = kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef;
            CXCursor ref = type || kind == CXCursor_DeclRefExpr ? clang_getCursorReferenced(k) : clang_getNullCursor();
            if (!clang_Cursor_isNull(ref))
            {
                // The type of the declaration itself, behind a pointer or a reference: the first
                // declaration of the type is sufficient.
                const bool decl_only = type && clang_equalCursors(c, var) && !clang_Cursor_isNull(pointee) &&
                    clang_equalCursors(clang_getCanonicalCursor(ref), pointee);
                CXCursor d = type && !decl_only ? clang_getCursorDefinition(ref) : clang_getCanonicalCursor(ref);
                if (clang_Cursor_isNull(d))
                    d = ref;
                const Entry* en = lookup(ref);
                // A pointer or a reference to a script class: the generated file declares the
                // class before the module block.
                if (decl_only && en && en->kind == Kind::ScriptClass)
                {
                    walk(k);
                    continue;
                }
                // "Player" in "int Player::x = 1;" is the qualifier of a static data member.
                const bool qualifier = type && en && en->kind == Kind::ScriptClass && qualifies_static(c) &&
                    scope_follows(k);
                const bool module_var =
                    qualifier || (en && (en->kind == Kind::ModuleVar || en->kind == Kind::ScriptStatic));
                // A static const member is a const before the module block of its class. A static
                // method has its prototype after the module block: the initializer cannot call it.
                uint32_t dpos = pos_of(d);
                if (en && en->kind == Kind::ScriptStaticConst)
                    dpos = en->cpos;
                else if (en && en->kind == Kind::ScriptStaticFn)
                    dpos = UINT32_MAX;
                if (!module_var && from_main(d) && dpos > first_var_pos_ && !own.has(dpos))
                {
                    const std::string first = "the first module variable '" + first_var_name_ + "'";
                    if (type)
                        fail(pos_of(k),
                            "the type '" + spelling(ref) + "' must come before " + first +
                                ": the module block is there and needs the complete type");
                    if (en && en->kind == Kind::ScriptStaticFn)
                        fail(pos_of(k),
                            "the initializer of a module variable cannot call the static method '" + en->shown() +
                                "' in a .cpp script: the code of a static method comes after the module block; "
                                "assign the value in a method or a function");
                    fail(pos_of(k),
                        "'" + spelling(ref) + "' must come before " + first +
                            ": the module block is there and holds the initializers");
                }
            }
            walk(k);
        }
    };
    walk(var);
}

// A variable that is not a module variable gets its value when the library loads. The module
// state does not exist then. So its initializer cannot use a module variable. The function
// examines the static variables in namespaces, in "extern" blocks and in plain classes too.
void Frontend::check_static_init(CXCursor c)
{
    const CXCursorKind kind = clang_getCursorKind(c);
    if (kind == CXCursor_VarDecl)
    {
        std::string var;
        uint32_t pos = 0;
        if (find_module_ref(c, nullptr, var, pos))
            fail(pos,
                "the variable '" + spelling(c) + "' cannot use the module variable '" + var +
                    "' in a .cpp script: it gets its value before the module state exists");
        return;
    }
    if (kind == CXCursor_Namespace || kind == CXCursor_LinkageSpec || is_record(kind))
        for (CXCursor k : children(c))
            check_static_init(k);
}
// Records the edit of one cursor in a slice. The cases of the switch are the rules. A cursor
// outside every slice gets no edit and no error. "st.slice" is the slice of the cursor before
// the first error.
void Frontend::examine(CXCursor k, WalkState& st)
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
    if (e.begin < st.skip_end)
        return; // inside a static member use: the edit of that use replaces the text
    if (!slice_at(e.begin, st.slice))
        return;
    if (st.cls != slices_[st.slice].cls)
        fail(e.begin, "internal error: the walk found a different class for this slice");
    const bool macro = in_macro(k);
    auto add = [&](uint32_t pos, uint32_t len, std::string rep, bool keep = false)
    {
        if (macro)
            fail(e.begin, kMacroMsg);
        edits_.push_back(Edit{pos, len, std::move(rep), keep});
    };
    // A default member initializer runs in a function of the state block, outside the object. Outside
    // a lambda it cannot use "this", a field or a method of the object.
    if (st.cls && !slices_[st.slice].field.empty() &&
        std::none_of(st.lambdas.begin(), st.lambdas.end(), [&](const Range& r) { return r.has(e.begin); }))
    {
        std::string used;
        if (kind == CXCursor_CXXThisExpr && word_at(text_, e.begin, "this"))
            used = "this";
        else if (kind == CXCursor_MemberRefExpr)
        {
            const CXCursor r = clang_getCursorReferenced(k);
            const CXCursorKind rk = clang_getCursorKind(r);
            const std::string name = spelling(r);
            // The object is implicit ("a") or "this->a"; a member of another object is valid.
            const bool own = word_at(text_, e.begin, "this") || text_.compare(e.begin, name.size(), name) == 0;
            if (own && (rk == CXCursor_FieldDecl || (rk == CXCursor_CXXMethod && !clang_CXXMethod_isStatic(r))))
                used = name;
        }
        if (!used.empty())
            fail(e.begin, "the default member initializer of the field '" + slices_[st.slice].field +
                    "' cannot use '" + used + "' in a .cpp script: the initializer runs outside the object; set '" +
                    slices_[st.slice].field + "' in the constructor");
    }
    // The implicit "operator=" of a script class copies the shell, so two shells share one state.
    if (kind == CXCursor_CallExpr)
    {
        const CXCursor r = clang_getCursorReferenced(k);
        if (clang_getCursorKind(r) == CXCursor_CXXMethod && spelling(r) == "operator=")
            if (const Entry* en = class_entry(clang_getCursorType(clang_getCursorSemanticParent(r))))
                fail(e.begin, "an assignment of an object of the script class '" + en->shown() +
                        "' is not supported in a .cpp script; assign the fields one by one");
    }
    // True when the text before "pos" ends with "::" (a qualified name).
    auto qualified = [&](uint32_t pos)
    {
        size_t i = pos;
        while (i > 0 && is_space(text_[i - 1]))
            --i;
        return i >= 2 && text_.compare(i - 2, 2, "::") == 0;
    };
    // The start of the qualifier before the name at "pos": "game::ai::Brain" starts at "game". The
    // generated name of a class in a namespace has no qualifier, so an edit replaces all of it.
    auto qualifier_begin = [&](size_t pos)
    {
        size_t b = pos;
        for (;;)
        {
            size_t i = b;
            while (i > 0 && is_space(text_[i - 1]))
                --i;
            if (i < 2 || text_.compare(i - 2, 2, "::") != 0)
                return b;
            const size_t colon = i - 2;
            i = colon;
            while (i > 0 && is_space(text_[i - 1]))
                --i;
            size_t j = i;
            while (j > 0 && is_ident(text_[j - 1]))
                --j;
            if (j == i)
                return colon; // a leading "::"
            b = j;
        }
    };
    // True when "ref" is a static member or a nested type of the native base or its ancestors.
    auto base_scoped = [&](CXCursor ref)
    {
        if (!st.cls)
            return false;
        CXCursor p = clang_getCursorSemanticParent(ref);
        if (clang_getCursorKind(p) == CXCursor_EnumDecl && !clang_EnumDecl_isScoped(p))
            p = clang_getCursorSemanticParent(p);
        return is_record(clang_getCursorKind(p)) && st.cls->base_usrs.count(usr(p)) != 0;
    };
    const std::string t = e.end > e.begin ? text_.substr(e.begin, e.end - e.begin) : std::string();
    // A use of a static member of a script class: the whole expression becomes the mangled name.
    // The object of "obj.name" is dropped, so it must have no side effect.
    auto rewrite_static = [&](CXCursor ref)
    {
        const Entry* en = lookup(ref);
        if (!en || (en->kind != Kind::ScriptStatic && en->kind != Kind::ScriptStaticConst &&
                       en->kind != Kind::ScriptStaticFn))
            return false;
        if (kind == CXCursor_MemberRefExpr)
            for (CXCursor c : children(k))
                if (!no_side_effects(c))
                    fail(e.begin, "a static member must be used by its name");
        add(e.begin, e.end - e.begin, (en->kind == Kind::ScriptStatic ? "hot_mod()->" : "") + en->mangled);
        st.skip_end = e.end;
        return true;
    };

    // True when the object "c" of a member access is "this" or "*this".
    std::function<bool(CXCursor)> is_this = [&](CXCursor c)
    {
        switch (clang_getCursorKind(c))
        {
        case CXCursor_CXXThisExpr:
            return true;
        case CXCursor_UnaryOperator:
            if (extent_of(c).begin >= text_.size() || text_[extent_of(c).begin] != '*')
                return false;
            [[fallthrough]];
        case CXCursor_ParenExpr:
        case CXCursor_UnexposedExpr:
        {
            const std::vector<CXCursor> ks = children(c);
            return ks.size() == 1 && is_this(ks[0]);
        }
        default:
            return false;
        }
    };
    // A field or a non-virtual method of a script class on an object other than "this". The
    // object is a pointer or a reference to the shell. A field is in the state of the object:
    // "p->f" becomes "state_of(p)->f". A method call becomes the call of the generated
    // function: "p->m(a)" becomes "C__m(p, a)". A reference object gives its address.
    // The object of the member access "k", or null for an implicit "this". A qualifier, for
    // example the "Player" of "Player::m", is not the object.
    auto object_of = [&]()
    {
        for (CXCursor c : children(k))
            if (clang_isExpression(clang_getCursorKind(c)))
                return c;
        return clang_getNullCursor();
    };
    // A call "m(a)", "this->m(a)" or "Player::m(a)" on the object itself becomes "callee(__self, a)".
    auto rewrite_self_call = [&](const std::string& callee)
    {
        const size_t paren = skip_ws(text_, e.end, text_.size());
        if (paren >= text_.size() || text_[paren] != '(')
            fail(pos_of(k), "the method '" + spelling(k) + "' of a script class must be called");
        const size_t first = skip_ws(text_, paren + 1, text_.size());
        const bool no_args = first < text_.size() && text_[first] == ')';
        add(e.begin, static_cast<uint32_t>(paren + 1 - e.begin), callee + "(__self" + (no_args ? "" : ", "));
        st.skip_end = std::max(st.skip_end, e.end);
    };
    // "callee" is the generated function of a method call. Empty: "<Owner>__<name>".
    auto rewrite_member = [&](const Entry& en, const std::string& callee = std::string())
    {
        const CXCursor first_obj = object_of();
        if (clang_Cursor_isNull(first_obj) || is_this(first_obj))
            return false;
        const Range o = extent_of(first_obj);
        const uint32_t name = pos_of(k);
        const std::string op = name > o.end ? trim(text_.substr(o.end, name - o.end)) : std::string();
        if (op != "->" && op != ".")
            fail(e.begin, "cannot read the member access of the script class '" + en.owner_shown() + "'");
        const bool arrow = op == "->";
        // A call of a const method adds an implicit const conversion of the object. The type
        // of the object is the type under that conversion.
        CXCursor obj = first_obj;
        for (;;)
        {
            if (clang_getCursorKind(obj) != CXCursor_UnexposedExpr)
                break;
            const std::vector<CXCursor> in = children(obj);
            if (in.size() != 1 || extent_of(in[0]).begin != o.begin || extent_of(in[0]).end != o.end)
                break;
            obj = in[0];
        }
        CXType ot = clang_getCursorType(obj);
        if (arrow)
            ot = clang_getPointeeType(ot);
        std::string open = arrow ? "" : "&(", close = arrow ? "" : ")";
        if (clang_isConstQualifiedType(ot)) // the source compiles, so the access does not write
        { // the shell of the object: the class of a child object can differ from the owner of the member
            const Entry* oc = class_entry(ot);
            open = "const_cast<" + (oc ? oc->name : en.owner_name) + "_Shell*>(" + open;
            close += ")";
        }
        if (en.kind == Kind::ScriptField)
        {
            // Optimized AOT: the script class is its shell, so "a.f" and "p->f" compile as they are.
            if (aot_optimized_)
                return true;
            add(o.begin, 0, "state_of(" + open, true);
            add(o.end, name - o.end, close + ")->", true);
            return true;
        }
        const size_t paren = skip_ws(text_, e.end, text_.size());
        if (paren >= text_.size() || text_[paren] != '(')
            fail(name, "the method '" + en.owner_shown() + "::" + en.name + "' of a script class must be called");
        const size_t first = skip_ws(text_, paren + 1, text_.size());
        const bool no_args = first < text_.size() && text_[first] == ')';
        add(o.begin, 0, (callee.empty() ? en.owner_name + "__" + en.name : callee) + "(" + open, true);
        add(o.end, static_cast<uint32_t>(paren + 1 - o.end), close + (no_args ? "" : ", "), true);
        return true;
    };

    // True when the cursor is inside a lambda. The state has no stable address there.
    const bool in_lambda = std::any_of(st.lambdas.begin(), st.lambdas.end(),
        [&](const Range& r) { return r.has(e.begin); });
    // The capture list of the lambda "lt" (its text) has "this": "[this]", "[&, this]" or "[=, this]".
    // The item "this" becomes "__self". The default "=" makes the item redundant, so it goes away.
    auto rewrite_this_capture = [&](const std::string& lt)
    {
        if (lt.empty() || lt[0] != '[')
            return;
        struct Item
        {
            size_t begin, end, comma; // the trimmed text of the item, and the comma before it
        };
        std::vector<Item> items;
        size_t start = 1, comma = std::string::npos, close = std::string::npos;
        int depth = 0;
        for (size_t i = 1; i < lt.size(); ++i)
        {
            const char ch = lt[i];
            if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if ((ch == ')' || ch == '}' || (ch == ']' && depth > 0)))
                --depth;
            else if ((ch == ',' || ch == ']') && depth == 0)
            {
                size_t b = skip_ws(lt, start, i), en = i;
                while (en > b && is_space(lt[en - 1]))
                    --en;
                if (en > b)
                    items.push_back(Item{b, en, comma});
                comma = i;
                start = i + 1;
                if (ch == ']')
                {
                    close = i;
                    break;
                }
            }
        }
        bool has_this = false;
        const bool copy_default = !items.empty() && lt.compare(items[0].begin, items[0].end - items[0].begin, "=") == 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const std::string item = lt.substr(items[i].begin, items[i].end - items[i].begin);
            if (item.size() > 1 && item[0] == '*' && trim(item.substr(1)) == "this")
                fail(e.begin + static_cast<uint32_t>(items[i].begin),
                    "a lambda cannot capture '*this' in a .cpp script: it would copy the object; capture a copy of the fields");
            // An init-capture "x = this": the new name is the shell, so "x->f" reads through the shell.
            const size_t eq = item.find('=');
            if (eq != std::string::npos && eq > 0 && item[eq - 1] != '!' && item[eq - 1] != '<' &&
                item[eq - 1] != '>' && (eq + 1 >= item.size() || item[eq + 1] != '=') &&
                trim(item.substr(eq + 1)) == "this")
            {
                add(e.begin + static_cast<uint32_t>(items[i].end) - 4, 4, "__self");
                continue;
            }
            if (item != "this")
                continue;
            has_this = true;
            if (copy_default && items[i].comma != std::string::npos)
                add(e.begin + static_cast<uint32_t>(items[i].comma), static_cast<uint32_t>(items[i].end - items[i].comma), "");
            else
                add(e.begin + static_cast<uint32_t>(items[i].begin), 4, "__self");
        }
        // The default "&" captures the local "__self" by reference. A stored lambda would keep a
        // dangling reference. So the capture "__self" by copy comes in, when the body uses the shell.
        const bool ref_default = !items.empty() && lt.compare(items[0].begin, items[0].end - items[0].begin, "&") == 0;
        if (ref_default && !has_this && close != std::string::npos)
            st.ref_lambdas.emplace_back(e, static_cast<uint32_t>(e.begin + close));
    };

    switch (kind)
    {
    case CXCursor_MemberRefExpr:
    {
        CXCursor ref = clang_getCursorReferenced(k);
        if (clang_Cursor_isNull(ref))
            return;
        if (rewrite_static(ref))
            return;
        const Entry* men = lookup(ref);
        const std::string parent = usr(clang_getCursorSemanticParent(ref));
        // "Player::m(a)" in a child of Player: the implementation that Player sees, with no
        // virtual dispatch. This is "base.m(a)" of a .hot child.
        const Entry* qual = nullptr;
        for (CXCursor c : children(k))
            if (clang_getCursorKind(c) == CXCursor_TypeRef && pos_of(c) < pos_of(k))
            {
                const Entry* en = lookup(clang_getCursorReferenced(c));
                qual = en && en->kind == Kind::ScriptClass ? en : qual;
            }
        if (qual)
        {
            const CXCursor obj = object_of();
            if (!st.cls || !st.cls->script_bases.count(qual->owner_usr) || (!clang_Cursor_isNull(obj) && !is_this(obj)))
                fail(pos_of(k),
                    "the script class '" + qual->shown() +
                        "' can qualify only a static member, or a member that a child class uses on itself, in a "
                        ".cpp script");
            const uint32_t name = pos_of(k);
            const bool method = clang_getCursorKind(ref) == CXCursor_CXXMethod;
            if (men && men->kind == Kind::ScriptMethod && !(men->virt && men->vowner.empty()))
                rewrite_self_call(men->owner_name + "__" + men->name);
            else if (method && clang_CXXMethod_isVirtual(ref)) // a native virtual, or an override of one
                rewrite_self_call(qual->name + "__base_" + spelling(ref));
            else
            { // a field of the state, or a member of the native base
                add(e.begin, name - e.begin, men && men->kind == Kind::ScriptField ? "this->" : "__self->");
                st.skip_end = std::max(st.skip_end, name);
            }
            return;
        }
        if (men && men->kind == Kind::ScriptMethod && !men->vowner.empty())
        { // a script virtual: the call reads the slot of the class with the "virtual" declaration
            const std::string callee = men->vowner + "__" + men->name + "_virt";
            const CXCursor obj = object_of();
            if (clang_Cursor_isNull(obj) || is_this(obj))
                rewrite_self_call(callee);
            else
                rewrite_member(*men, callee);
            return;
        }
        if (men && (men->kind == Kind::ScriptField || (men->kind == Kind::ScriptMethod && !men->virt)) &&
            rewrite_member(*men))
            return;
        if (!st.cls)
            return;
        // An override of a native virtual in this class or in a script parent: the native call
        // on the shell reads the slot.
        const bool native_override = men && men->kind == Kind::ScriptMethod && men->virt &&
            (men->owner_usr == st.cls->usr || st.cls->script_bases.count(men->owner_usr) != 0);
        const bool need = st.cls->base_usrs.count(parent) != 0 || native_override;
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
            st.this_member.insert(e.begin);
        // A field or a script method of the object itself in a lambda: the state of the shell.
        if (in_lambda && men &&
            (men->kind == Kind::ScriptField || (men->kind == Kind::ScriptMethod && !men->virt)))
        {
            if (explicit_this)
                add(e.begin, 4, aot_optimized_ ? "__self" : "state_of(__self)");
            else
                add(e.begin, 0, aot_optimized_ ? "__self->" : "state_of(__self)->");
            return;
        }
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
        if (!st.cls || st.this_member.count(e.begin) || !word_at(text_, e.begin, "this"))
            return; // a member access uses it, or the "this" is implicit
        // The "this" of a local class in the body is not the script object.
        if (usr(clang_getTypeDeclaration(clang_getPointeeType(clang_getCursorType(k)))) != st.cls->usr)
            return;
        add(e.begin, 4, "__self");
        return;
    }
    case CXCursor_DeclRefExpr:
    {
        CXCursor ref = clang_getCursorReferenced(k);
        if (clang_Cursor_isNull(ref))
            return;
        if (rewrite_static(ref))
            return;
        const Entry* en = lookup(ref);
        if (en && en->kind == Kind::ModuleVar)
        {
            add(e.begin, e.end - e.begin, "hot_mod()->" + en->name);
            return;
        }
        // A local of an enclosing function in a lambda (see the check of stored lambdas).
        const CXCursorKind rk = clang_getCursorKind(ref);
        if (in_lambda && (rk == CXCursor_VarDecl || rk == CXCursor_ParmDecl))
        {
            const CXCursorKind pk = clang_getCursorKind(clang_getCursorSemanticParent(ref));
            const bool local = pk == CXCursor_FunctionDecl || pk == CXCursor_CXXMethod || pk == CXCursor_Constructor ||
                pk == CXCursor_Destructor || pk == CXCursor_FunctionTemplate || pk == CXCursor_LambdaExpr;
            const uint32_t at = extent_of(ref).begin;
            // Each lambda around the use that does not hold the declaration captures the local.
            for (const Range& r : st.lambdas)
                if (local && r.has(e.begin) && !r.has(at))
                    st.outer_locals.emplace_back(r.begin, spelling(ref), e.begin);
        }
        if (t.find("::") == std::string::npos && !qualified(e.begin) && base_scoped(ref))
            add(e.begin, 0, st.cls->native + "::");
        return;
    }
    case CXCursor_TypeRef:
    {
        CXCursor ref = clang_getCursorReferenced(k);
        const Entry* en = lookup(ref);
        if (en && en->kind == Kind::ScriptClass)
        { // check_type_refs allows only a pointer, a reference, sizeof and a static qualifier
            if (!scope_follows(k))
            {
                const uint32_t from = en->display == en->name ? e.begin : static_cast<uint32_t>(qualifier_begin(e.begin));
                add(from, e.end - from, en->name + "_Shell");
            }
            return;
        }
        if (!qualified(e.begin) && base_scoped(ref))
            add(e.begin, 0, st.cls->native + "::");
        return;
    }
    case CXCursor_CXXNewExpr:
    { // "new C(args)" becomes "C__new(args)": the factory of the class (the same as in .hot)
        const Entry* en = class_entry(clang_getPointeeType(clang_getCursorType(k)));
        if (!en)
            return;
        Range tr{};
        for (CXCursor c : children(k))
            if (clang_getCursorKind(c) == CXCursor_TypeRef && lookup(clang_getCursorReferenced(c)) == en)
            {
                tr = extent_of(c);
                break;
            }
        if (tr.end <= e.begin || tr.end > e.end)
            fail(e.begin, "cannot read this 'new' of the script class '" + en->shown() + "'");
        add(e.begin, tr.end - e.begin, en->name + "__new");
        const size_t i = skip_ws(text_, tr.end, e.end);
        if (i >= e.end)
            add(tr.end, 0, "()");
        else if (text_[i] == '{' && text_[e.end - 1] == '}')
        {
            add(static_cast<uint32_t>(i), 1, "(");
            add(e.end - 1, 1, ")");
        }
        st.skip_end = std::max(st.skip_end, tr.end);
        return;
    }
    case CXCursor_CXXDynamicCastExpr:
    { // the class records give the answer, not the C++ RTTI (see doc/cpp-scripts.md)
        CXType cast_type = clang_getCursorType(k);
        const bool ptr = cast_type.kind == CXType_Pointer;
        if (cast_type.kind == CXType_Pointer || cast_type.kind == CXType_LValueReference || cast_type.kind == CXType_RValueReference)
            cast_type = clang_getPointeeType(cast_type);
        const Entry* en = class_entry(cast_type);
        if (!en)
            return;
        const size_t lt = text_.find('<', e.begin);
        const size_t gt = lt == std::string::npos ? lt : match_close(text_, lt, e.end);
        const size_t open = gt == std::string::npos ? gt : skip_ws(text_, gt + 1, e.end);
        if (open >= e.end || text_[open] != '(' || text_[e.end - 1] != ')')
            fail(e.begin, "cannot read this dynamic_cast to the script class '" + en->shown() + "'");
        const std::string shell = en->name + "_Shell", quoted = "\"" + en->shown() + "\"";
        const uint32_t head = static_cast<uint32_t>(open + 1 - e.begin);
        if (ptr)
        {
            add(e.begin, head, "hot::as_script<" + shell + ">(");
            add(e.end - 1, 1, ", " + quoted + ")");
        }
        else
        { // a reference: an object of another class throws std::bad_cast, as in C++
            add(e.begin, head, "hot::cast_ref<" + shell + ">(");
            add(e.end - 1, 1, ", " + quoted + ")");
        }
        st.skip_end = std::max(st.skip_end, static_cast<uint32_t>(open + 1));
        return;
    }
    case CXCursor_LambdaExpr:
    {
        // A lambda must not keep the pointer to the state: a reload moves the state to a new block.
        // So the body reaches a field through the shell (see in_lambda), and the capture "this"
        // becomes the capture "__self". The shell keeps its address.
        st.lambdas.push_back(e);
        st.lambda_owner.push_back(st.cls ? st.cls->display : std::string());
        rewrite_this_capture(t);
        return;
    }
    case CXCursor_CallExpr:
    {
        // "field = [&] { ... }": the lambda goes into a field or a module variable and lives after
        // the method. apply_slices checks that it holds no reference to a local.
        if (spelling(k) != "operator=")
            return;
        const std::vector<CXCursor> kids = children(k);
        if (kids.size() < 2)
            return;
        CXCursor lhs = kids[0];
        while (clang_getCursorKind(lhs) == CXCursor_UnexposedExpr && children(lhs).size() == 1)
            lhs = children(lhs)[0];
        const CXCursor target = clang_getCursorReferenced(lhs);
        const CXCursorKind tk = clang_getCursorKind(target);
        const CXCursorKind tp = clang_getCursorKind(clang_getCursorSemanticParent(target));
        const bool lasting = tk == CXCursor_FieldDecl ||
            (tk == CXCursor_VarDecl && (tp == CXCursor_TranslationUnit || tp == CXCursor_Namespace)) ||
            (tk == CXCursor_VarDecl && clang_Cursor_getStorageClass(target) == CX_SC_Static);
        if (!lasting)
            return;
        // The first lambda of the right side (a std::function takes it through a conversion).
        std::vector<CXCursor> todo(kids.begin() + 1, kids.end());
        for (size_t i = 0; i < todo.size() && i < 64; ++i)
        {
            if (clang_getCursorKind(todo[i]) == CXCursor_LambdaExpr)
            {
                st.stored_lambdas.insert(extent_of(todo[i]).begin);
                break;
            }
            for (CXCursor c : children(todo[i]))
                todo.push_back(c);
        }
        return;
    }
    default:
        return;
    }
}

// Examines the cursor "k" and then the cursors below it. The parent comes first: a member
// access marks its "this". An error goes to the slice of the cursor. The walk continues,
// because an error of a later slice must not hide an error of an earlier slice.
void Frontend::visit(CXCursor k, WalkState& st)
{
    try
    {
        examine(k, st);
    }
    catch (CppFail& f)
    {
        std::optional<CppFail>& first = st.fails[st.slice];
        if (!first)
            first = std::move(f);
    }
    for (CXCursor c : children(k))
        visit(c, st);
}

// The one walk of the main file. It visits each top-level cursor and all cursors below it once.
// A script class is a top-level cursor. The index gives it, and it sets the class of the walk.
// A preprocessing cursor (for example a macro expansion in a body) is a top-level cursor too. It
// is not below a declaration and no rule applies to it, so the walk skips it.
void Frontend::walk_main(WalkState& st)
{
    for (CXCursor k : top_)
    {
        if (clang_isPreprocessing(clang_getCursorKind(k)))
            continue;
        st.cls = nullptr;
        const Entry* en = clang_isCursorDefinition(k) ? lookup(k) : nullptr;
        if (en && en->kind == Kind::ScriptClass)
        {
            auto it = class_infos_.find(usr(k));
            if (it != class_infos_.end())
                st.cls = it->second;
        }
        else if (is_member_def(k))
        { // a definition outside the class: the body is the body of a member
            auto it = class_infos_.find(usr(clang_getCursorSemanticParent(k)));
            if (it != class_infos_.end())
                st.cls = it->second;
        }
        visit(k, st);
    }
}

void Frontend::add_slice(Range extent, const ClassInfo* cls, std::function<void(std::string&&)> store)
{
    slices_.push_back(Slice{extent, cls, std::move(store)});
}

// Finds the slice whose extent holds "pos". The extents do not overlap.
bool Frontend::slice_at(uint32_t pos, size_t& out) const
{
    auto it = std::upper_bound(by_pos_.begin(), by_pos_.end(), pos,
        [this](uint32_t p, size_t i) { return p < slices_[i].extent.begin; });
    if (it == by_pos_.begin() || !slices_[*(it - 1)].extent.has(pos))
        return false;
    out = *(it - 1);
    return true;
}

// Walks the main file once. Then it edits the slices in the order of the records. So the first
// error is the same as with an edit at the time of the record. In each slice, the edits apply
// from the last offset to the first offset.
void Frontend::apply_slices()
{
    by_pos_.clear();
    for (size_t i = 0; i < slices_.size(); ++i)
        if (slices_[i].extent.end > slices_[i].extent.begin) // an empty slice holds no cursor
            by_pos_.push_back(i);
    std::stable_sort(by_pos_.begin(), by_pos_.end(),
        [this](size_t a, size_t b) { return slices_[a].extent.begin < slices_[b].extent.begin; });
    for (size_t i = 1; i < by_pos_.size(); ++i)
        if (slices_[by_pos_[i]].extent.begin < slices_[by_pos_[i - 1]].extent.end)
            fail(slices_[by_pos_[i]].extent.begin, "internal error: two edit slices overlap");
    WalkState st;
    st.fails.resize(slices_.size());
    edits_ = fixed_edits_;
    walk_main(st);
    for (const auto& [range, close] : st.ref_lambdas)
    {
        bool uses_self = false;
        for (const Edit& ed : edits_)
            if (range.has(ed.pos) && ed.pos != close && ed.text.find("__self") != std::string::npos)
                uses_self = true;
        if (uses_self)
            edits_.push_back(Edit{close, 0, ", __self", false});
    }
    // A stored lambda must not hold a reference to a local: the method returns, and the local is
    // gone. The capture default "&" or an item "&x" with a use of a local of an enclosing function
    // is an error.
    auto report = [&](uint32_t pos, const std::string& msg)
    {
        size_t si = 0;
        if (!slice_at(pos, si) || st.fails[si])
            return;
        try
        {
            fail(pos, msg);
        }
        catch (CppFail& f)
        {
            st.fails[si] = std::move(f);
        }
    };
    for (const auto& [begin, name, at] : st.outer_locals)
    {
        if (!st.stored_lambdas.count(begin))
            continue;
        const size_t close = text_.find(']', begin);
        if (close == std::string::npos)
            continue;
        // The items of the capture list, without spaces.
        std::vector<std::string> items;
        std::string cur;
        for (size_t i = begin + 1; i < close; ++i)
        {
            if (text_[i] == ',')
            {
                items.push_back(cur);
                cur.clear();
            }
            else if (!is_space(text_[i]))
                cur += text_[i];
        }
        items.push_back(cur);
        bool by_ref = false;
        bool by_copy = false;
        for (const std::string& it : items)
        {
            by_ref = by_ref || it == "&" + name;
            by_copy = by_copy || it == name;
        }
        if (!items.empty() && items[0] == "&" && !by_copy)
            by_ref = true;
        if (by_ref)
            report(at,
                "the lambda goes into a field or a module variable and keeps a reference to the local '" + name +
                    "': the local ends when the function returns; capture it by copy, for example [" + name + "]");
    }
    // A lambda that uses the object or a module variable gets the guard hot::cpp_lambda. After a
    // reload its code reads an old layout and a free module block, so the guard skips the call.
    if (!aot_optimized_)
    {
        const size_t n = edits_.size();
        for (size_t li = 0; li < st.lambdas.size(); ++li)
        {
            const Range& r = st.lambdas[li];
            bool uses_state = false;
            for (size_t i = 0; i < n && !uses_state; ++i)
            {
                const Edit& ed = edits_[i];
                uses_state = r.has(ed.pos) && (ed.text.find("__self") != std::string::npos ||
                                                  ed.text.find("state_of(") != std::string::npos ||
                                                  ed.text.find("hot_mod()") != std::string::npos);
            }
            if (!uses_state)
                continue;
            const std::string& owner = st.lambda_owner[li];
            const std::string what = (owner.empty() ? std::string() : owner + " ") + "lambda";
            edits_.push_back(Edit{r.begin, 0, "hot::cpp_lambda(&hot_live, \"" + what + "\", ", true});
            edits_.push_back(Edit{r.end, 0, ")", true});
        }
    }
    std::vector<std::vector<const Edit*>> edits(slices_.size()); // the edits of each slice, in walk order
    for (const Edit& ed : edits_)
    {
        size_t si = 0;
        // An insertion at the end of a slice (the ")" of a lambda guard) belongs to that slice.
        if (!slice_at(ed.pos, si) && !(ed.len == 0 && ed.pos > 0 && slice_at(ed.pos - 1, si)))
            fail(ed.pos, "internal error: an edit is outside every slice");
        edits[si].push_back(&ed);
    }
    for (size_t i = 0; i < slices_.size(); ++i)
    {
        if (st.fails[i])
            throw *st.fails[i];
        std::vector<const Edit*>& eds = edits[i];
        // From the last offset to the first. At one offset, a replacement comes before an
        // insertion. The insertions at one offset apply in the reverse order of the walk, so
        // the insertion of an outer expression comes first in the text: "state_of(state_of(a)->b)".
        std::stable_sort(eds.begin(), eds.end(),
            [](const Edit* a, const Edit* b)
            {
                if (a->pos != b->pos)
                    return a->pos > b->pos;
                if ((a->len == 0) != (b->len == 0))
                    return a->len != 0;
                return a->len == 0 && a > b; // the edits are in one vector in the order of the walk
            });
        const Range region = slices_[i].extent;
        std::string s = text_.substr(region.begin, region.end - region.begin);
        const Edit* prev = nullptr;
        for (const Edit* ed : eds)
        {
            if (prev && !ed->keep && prev->pos == ed->pos && prev->len == ed->len && prev->text == ed->text)
                continue; // the same edit twice
            s.replace(ed->pos - region.begin, ed->len, ed->text);
            prev = ed;
        }
        slices_[i].store(std::move(s));
    }
    slices_.clear();
    by_pos_.clear();
    edits_.clear();
    fixed_edits_.clear();
}

// A field or a module variable. The initializer is the text after "=", or "{...}", or "(...)".
// The function adds the field to the list that "where" gives. "mangled" is the name of a static
// data member as a module variable: "<Class>__s_<name>".
void Frontend::make_field(CXCursor c, const ClassInfo* cls, const Where<ast::Field>& where, const std::string& mangled)
{
    std::vector<ast::Field>& out = where();
    out.emplace_back();
    const size_t index = out.size() - 1;
    ast::Field& f = out.back();
    f.raw = true;
    f.name = mangled.empty() ? spelling(c) : mangled;
    f.type = f.script_type = type_in_script(clang_getCursorType(c));
    f.pos = pos_of(c);
    f.line = line(f.pos);
    if (!cls || !mangled.empty())
        f.ns = ns_of(c); // a module variable: the module block is at file scope
    switch (mangled.empty() ? clang_getCXXAccessSpecifier(c) : CX_CXXPublic)
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
            if (a.rfind("hot.alias=", 0) == 0)
                f.alias = a.substr(std::strlen("hot.alias="));
        }
    read_init(c, cls, where, index);
}

// Reads the initializer of the declaration "c" for the field "index". The text finds the
// initializer. libclang does not visit every initializer: it skips the "{...}" of a class-type
// field. The edits of such an initializer are then missing.
void Frontend::read_init(CXCursor c, const ClassInfo* cls, const Where<ast::Field>& where, size_t index)
{
    ast::Field& f = where()[index];
    const Range decl = extent_of(c);
    size_t i = pos_of(c) + spelling(c).size();
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
        { // a macro attribute: HOT_ALIAS("old")
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
        add_slice(Range{start, decl.end}, cls, [where, index, open, fpos](std::string&& s) {
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
        if (cls)
            slices_.back().field = f.name;
    }
}

// True when the expression at "r" is the size of an array in a declarator, as in "int (*p)[3]".
// The size is a child of the parameter, but it is not a default value.
bool Frontend::in_array_bound(Range r) const
{
    size_t i = r.begin;
    while (i > 0 && is_space(text_[i - 1]))
        --i;
    return i > 0 && text_[i - 1] == '[';
}

void Frontend::fill_params(CXCursor c, std::vector<ast::Param>& out, const ClassInfo* cls)
{
    const int n = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < n; ++i)
    {
        CXCursor a = clang_Cursor_getArgument(c, static_cast<unsigned>(i));
        ast::Param p;
        p.name = spelling(a);
        p.type = type_in_script(clang_getCursorType(a));
        for (CXCursor k : children(a))
            if (clang_isExpression(clang_getCursorKind(k)) && !in_array_bound(extent_of(k)))
            {
                // The text is already C++. The edits are the same as in a body. The shared
                // default is "lowered", so the .hot path ("default_arg") writes the text as is.
                p.def = std::make_shared<ast::ParamDefault>();
                p.def->lowered = true;
                p.def->pos = pos_of(k);
                std::shared_ptr<ast::ParamDefault> def = p.def;
                add_slice(extent_of(k), cls, [def](std::string&& s) { def->cpp = trim(s); });
                break;
            }
        out.push_back(std::move(p));
    }
}

// The body of the function "c". The body of a coroutine (co_await, co_return) is in a
// CoroutineBodyStmt: libclang shows it as an unexposed statement, and its first child is the body.
static bool find_body(CXCursor c, CXCursor& out, bool* coroutine = nullptr)
{
    for (CXCursor k : children(c))
    {
        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_CompoundStmt)
        {
            out = k;
            return true;
        }
        if (kind == CXCursor_UnexposedStmt)
            for (CXCursor b : children(k))
            {
                if (clang_getCursorKind(b) == CXCursor_CompoundStmt)
                {
                    out = b;
                    if (coroutine)
                        *coroutine = true;
                    return true;
                }
                break; // the body is the first child
            }
    }
    return false;
}

CXCursor Frontend::body_of(CXCursor c)
{
    CXCursor b;
    if (find_body(c, b))
        return b;
    fail(pos_of(c), "define the static method inside the class");
}

bool Frontend::has_body(CXCursor c)
{
    CXCursor b;
    return find_body(c, b);
}

// True when the declaration "c" ends with "= default" or "= delete".
bool Frontend::is_defaulted(CXCursor c)
{
    const Range e = extent_of(c);
    size_t end = e.end;
    while (end > e.begin && is_space(text_[end - 1]))
        --end;
    for (const char* w : {"default", "delete"})
    {
        const size_t n = std::strlen(w);
        if (end >= e.begin + n && text_.compare(end - n, n, w) == 0)
        {
            size_t q = end - n;
            while (q > e.begin && is_space(text_[q - 1]))
                --q;
            if (q > e.begin && text_[q - 1] == '=')
                return true;
        }
    }
    return false;
}

// True when "c" is a method, a constructor or a destructor of a script class that the file
// defines outside the class.
bool Frontend::is_member_def(CXCursor c) const
{
    const CXCursorKind kind = clang_getCursorKind(c);
    if (kind != CXCursor_CXXMethod && kind != CXCursor_Constructor && kind != CXCursor_Destructor)
        return false;
    const CXCursorKind lexical = clang_getCursorKind(clang_getCursorLexicalParent(c));
    if (lexical != CXCursor_TranslationUnit && lexical != CXCursor_Namespace)
        return false;
    auto it = table_.find(usr(clang_getCursorSemanticParent(c)));
    return it != table_.end() && it->second.kind == Kind::ScriptClass;
}

// Records a member that the class declares without a body. The definition outside the class
// gives the body later (see define_member).
void Frontend::declare(
    CXCursor c, const ClassInfo* cls, size_t index, const Where<ast::Method>& m, const Where<ast::Ctor>& ct)
{
    Declared d;
    d.method = m;
    d.ctor = ct;
    d.index = index;
    d.cls = cls;
    d.pos = pos_of(c);
    d.name = cls->display + "::" + spelling(c);
    const CXCursorKind kind = clang_getCursorKind(c);
    d.what = kind == CXCursor_Constructor ? "constructor" : kind == CXCursor_Destructor ? "destructor" : "method";
    declared_[usr(c)] = std::move(d);
}

// The definition "c" outside the class gives the body of the member that the class declares. The
// parameter names of the definition are the names that the body uses. The types and the default
// arguments come from the declaration. The text of the definition does not stay in the output.
void Frontend::define_member(CXCursor c)
{
    if (pure_usrs_.count(usr(c)))
        fail(pos_of(c), "a pure virtual method cannot have a definition in a .cpp script class");
    auto it = declared_.find(usr(c));
    if (it == declared_.end())
        fail(pos_of(c),
            "the definition of '" + spelling(clang_getCursorSemanticParent(c)) + "::" + spelling(c) +
                "' has no declaration in the script class");
    Declared& d = it->second;
    if (d.defined)
        fail(pos_of(c), "the " + d.what + " '" + d.name + "' is defined twice");
    d.defined = true;
    check_late_decls(c, d, extent_of(c).begin);
    std::vector<ast::Param>& ps = d.ctor ? d.ctor()[d.index].params : d.method()[d.index].params;
    const int n = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < n && static_cast<size_t>(i) < ps.size(); ++i)
    {
        const std::string name = spelling(clang_Cursor_getArgument(c, static_cast<unsigned>(i)));
        if (!name.empty())
            ps[static_cast<size_t>(i)].name = name;
    }
    if (d.ctor)
        read_ctor_body(c, *d.cls, d.ctor, d.index);
    else
        read_method_body(c, d.cls, d.method, d.index);
}

// The body of a definition outside the class moves into the code of the class. So it cannot
// name a declaration that the file puts after the class and before the definition. "from" is
// the start of the definition. The first declaration of the name decides (a prototype before the
// class is valid). A script entity is not checked: the generated code has its own order for it.
void Frontend::check_late_decls(CXCursor c, const Declared& d, uint32_t from)
{
    for (CXCursor k : children(c))
    {
        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef || kind == CXCursor_NamespaceRef ||
            kind == CXCursor_DeclRefExpr)
        {
            CXCursor ref = clang_getCursorReferenced(k);
            if (!clang_Cursor_isNull(ref) && !lookup(ref))
            {
                ref = clang_getCanonicalCursor(ref);
                const uint32_t at = pos_of(ref);
                if (from_main(ref) && at >= d.cls->end && at < from && !lookup(ref))
                    fail(pos_of(k),
                        "the definition of '" + d.name + "' uses '" + spelling(ref) +
                            "', which the file declares after the class; the body moves into the class, so "
                            "declare '" + spelling(ref) + "' before the class");
            }
        }
        check_late_decls(k, d, from);
    }
}

// A member that the class declares and the file never defines is an error: the compiler sees it
// only at link time.
void Frontend::check_all_defined()
{
    const Declared* first = nullptr;
    for (const auto& kv : declared_)
        if (!kv.second.defined && (!first || kv.second.pos < first->pos))
            first = &kv.second;
    if (first)
        fail(first->pos,
            "the " + first->what + " '" + first->name +
                "' is declared but never defined; define it in the class or after the class");
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
        m.is_const = clang_CXXMethod_isConst(c) != 0;
        // An operator gets a safe identifier as the name. A defaulted or deleted operator gets the
        // message of the missing body below.
        const std::string& sp = m.name;
        if (sp.compare(0, 8, "operator") == 0 && (sp.size() == 8 || !is_ident(sp[8])) && !is_defaulted(c))
        {
            size_t b = 8, e = sp.size();
            while (b < e && is_space(sp[b]))
                ++b;
            while (e > b && is_space(sp[e - 1]))
                --e;
            const std::string sym = sp.substr(b, e - b);
            const int arity = clang_Cursor_getNumArguments(c);
            const std::optional<std::string> ident = cpp_op_ident(sym, arity < 0 ? 0 : static_cast<size_t>(arity));
            if (!ident)
                fail(m.pos, "operator '" + sym + "' is not supported in a .cpp script class");
            m.op = sym;
            m.name = *ident;
        }
    }
    if (kind == CXCursor_CXXMethod && !clang_CXXMethod_isStatic(c)) // a static convert hook has no "this"
    {
        m.is_override = has_child(c, CXCursor_CXXOverrideAttr);
        if (clang_CXXMethod_isVirtual(c) && !m.is_override)
        { // a new script virtual method: it has a slot, as a "virtual" method of a .hot class
            CXCursor* over = nullptr;
            unsigned n = 0;
            clang_getOverriddenCursors(c, &over, &n);
            clang_disposeOverriddenCursors(over);
            if (n != 0)
                fail(m.pos, "the method '" + m.name + "' overrides a virtual method; write 'override'");
            m.is_virtual = true;
        }
        // A method of a child hides the method of the parent with the same name, as in C++.
        // The .hot spelling of this is "new".
        if (cls && !clang_CXXMethod_isVirtual(c))
            for (const auto& v : table_)
                if (v.second.kind == Kind::ScriptMethod && v.second.name == m.name &&
                    cls->script_bases.count(v.second.owner_usr))
                    m.is_new = true;
    }
    if (kind != CXCursor_Destructor)
        m.ret = type_in_script(clang_getResultType(clang_getCursorType(c)));
    fill_params(c, m.params, cls);
    if (m.name == "onReload")
        for (ast::Param& p : m.params)
        {
            std::string t;
            for (char ch : p.type)
                if (!is_space(ch))
                    t += ch;
            if (t == "hot::OldState" || t == "consthot::OldState&" || t == "OldState" || t == "constOldState&")
                p.type = "OldState";
        }
    if (!has_body(c))
    {
        // A static method and a static convert hook need the body in the class.
        if (!cls || clang_CXXMethod_isStatic(c))
            fail(m.pos, "define the static method inside the class");
        if (clang_isCursorDefinition(c) || is_defaulted(c))
            fail(m.pos, "a defaulted or deleted method is not supported in a .cpp script class");
        if (clang_CXXMethod_isPureVirtual(c))
        { // "virtual int F(int) = 0;": an abstract method. It has a slot and no definition.
            if (m.is_override)
                fail(m.pos, "a pure virtual method cannot be 'override' in a .cpp script class");
            m.is_abstract = true;
            m.is_virtual = true;
            pure_usrs_.insert(usr(c));
            return;
        }
        declare(c, cls, index, where, nullptr);
        return;
    }
    read_method_body(c, cls, where, index);
}

// A "static" or "thread_local" local variable of a method or of a free function lives in the code
// of the module. A
// reload loads a new module, so the variable gets its initial value again. A const variable
// keeps its value, so it gets no warning.
void Frontend::warn_static_locals(CXCursor fn)
{
    std::function<void(CXCursor)> visit_node = [&](CXCursor n) {
        for (CXCursor k : children(n))
        {
            if (clang_getCursorKind(k) == CXCursor_VarDecl && !clang_isConstQualifiedType(clang_getCursorType(k)))
            {
                // The words before the name: "static", "thread_local" or both.
                const size_t b = extent_of(k).begin, e = std::min<size_t>(pos_of(k), text_.size());
                bool is_static = false, is_tls = false;
                for (size_t i = b; i < e;)
                {
                    if (!is_ident(text_[i]))
                    {
                        ++i;
                        continue;
                    }
                    size_t j = i;
                    while (j < e && is_ident(text_[j]))
                        ++j;
                    const std::string w = text_.substr(i, j - i);
                    is_static = is_static || w == "static";
                    is_tls = is_tls || w == "thread_local";
                    i = j;
                }
                if (is_static || is_tls)
                    warnings_.emplace_back(pos_of(k),
                        std::string("the ") + (is_tls ? "thread_local" : "static") + " local variable '" + spelling(k) +
                            "' resets to its initial value when the script reloads: a reload loads a new module");
            }
            visit_node(k);
        }
    };
    visit_node(fn);
}

// Warnings for the text that stays as written: a class template or a partial specialization with a
// base class, and a free function with a static local variable. A namespace that holds no script
// code stays one declaration, so the function looks into it.
void Frontend::warn_plain_text(CXCursor k)
{
    const CXCursorKind kind = clang_getCursorKind(k);
    if (kind == CXCursor_Namespace)
    {
        for (CXCursor c : children(k))
            if (from_main(c))
                warn_plain_text(c);
    }
    else if ((kind == CXCursor_ClassTemplate || kind == CXCursor_ClassTemplatePartialSpecialization) &&
        clang_isCursorDefinition(k) && has_child(k, CXCursor_CXXBaseSpecifier))
        warnings_.emplace_back(pos_of(k),
            "the class template '" + spelling(k) +
                "' has a base class, but it is not a script class: it is copied as plain C++ and has no hot reload");
    else if (kind == CXCursor_FunctionDecl && clang_isCursorDefinition(k))
        warn_static_locals(k);
}

// Reads the body of the method "index" from the definition "c". The definition is in the class
// or outside it.
void Frontend::read_method_body(CXCursor c, const ClassInfo* cls, const Where<ast::Method>& where, size_t index)
{
    ast::Method& m = where()[index];
    const Range b = extent_of(body_of(c));
    warn_static_locals(c);
    {
        CXCursor body;
        bool co = false;
        find_body(c, body, &co);
        m.is_coroutine = co;
    }
    m.end_pos = b.end - 1;
    m.end_line = line(m.end_pos);
    m.raw_line = line(b.begin); // "raw_body" starts right after the "{"
    add_slice(Range{b.begin + 1, b.end - 1}, cls,
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
    if (!has_body(c))
    {
        if (clang_isCursorDefinition(c) || is_defaulted(c))
            fail(ct.pos, "a defaulted or deleted constructor is not supported in a .cpp script class");
        declare(c, &cls, index, nullptr, where);
        return;
    }
    read_ctor_body(c, cls, where, index);
}

// Reads the initializer list that starts at the ":" in "colon". The entry of the native base keeps
// its handling. The entry of a field becomes a statement at the start of the body: the fields live
// in the state, so the state cannot construct them in the list. C++ initializes the fields in the
// order of the declarations, so the statements have this order. The statement makes the value
// with the direct initialization of the list, and then moves it into the field.
void Frontend::read_member_inits(
    CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where, size_t index, uint32_t colon, Range b)
{
    struct Item
    {
        std::string name;
        bool brace;
        uint32_t decl; // the offset of the declaration of the field
        uint32_t open, close;
    };
    std::vector<Item> items;
    const CXCursor owner = clang_getCursorSemanticParent(c);
    bool has_base = false;
    // Skips the white space and the comments between the entries.
    auto skip = [this, &b](size_t i)
    {
        for (;;)
        {
            i = skip_ws(text_, i, b.begin);
            const size_t k = i < b.begin ? skip_opaque(text_, i) : i;
            if (k == i || text_[i] == '"' || text_[i] == '\'')
                return i;
            i = k;
        }
    };
    size_t p = colon + 1;
    for (;;)
    {
        p = skip(p);
        const size_t s = p;
        size_t n = s;
        while (n < b.begin && (is_ident(text_[n]) || text_[n] == ':' || text_[n] == '<' || text_[n] == '>'))
            ++n;
        const std::string word = text_.substr(s, n - s);
        const size_t a = skip(n);
        const size_t a_end = a < b.begin && (text_[a] == '(' || text_[a] == '{') ? match_close(text_, a, b.begin)
                                                                                 : std::string::npos;
        if (word.empty() || a_end == std::string::npos)
            fail(static_cast<uint32_t>(s), "cannot read this member initializer");
        const size_t q = skip(a_end + 1);
        if (text_.compare(q, 3, "...") == 0)
            fail(static_cast<uint32_t>(q), "a pack expansion in a member initializer is not supported in a .cpp script");
        if (q != b.begin && text_[q] != ',')
            fail(static_cast<uint32_t>(q), "cannot read this member initializer");
        const Range args{static_cast<uint32_t>(a + 1), static_cast<uint32_t>(a_end)};
        if (last_name(word) == cls.simple)
            fail(static_cast<uint32_t>(s), "a delegating constructor is not supported in a .cpp script class");
        if (!cls.base.empty() && last_name(word) == cls.base_simple)
        {
            if (has_base)
                fail(static_cast<uint32_t>(s), "the base class is initialized twice in this constructor");
            has_base = true;
            ast::Ctor& ct = where()[index];
            ct.has_base_call = true;
            ct.base_pos = static_cast<uint32_t>(s);
            // The number of arguments. The translator selects the constructor of a script parent by
            // this number. The construct expression of the initializer has the arguments as children:
            // a comma in a template argument list, as in "T<a, b>::v", is not a separator. A default
            // argument of the parent has no place in "args", so it does not count.
            bool from_ast = false;
            for (CXCursor x : children(c))
            {
                const Range xr = extent_of(x);
                if (!clang_isExpression(clang_getCursorKind(x)) || xr.begin >= args.begin || xr.end < args.end)
                    continue;
                from_ast = true;
                for (CXCursor y : children(x))
                {
                    const Range yr = extent_of(y);
                    if (yr.begin >= args.begin && yr.end <= args.end && yr.end > yr.begin)
                        ++ct.raw_base_argc;
                }
                break;
            }
            // Without the expression (for example in a template), count the commas outside brackets,
            // strings and comments.
            int depth = 0;
            bool any = false;
            for (size_t i = args.begin; !from_ast && i < args.end;)
            {
                const size_t k = skip_opaque(text_, i);
                if (k > i)
                {
                    any = any || (text_[i] != '/');
                    i = k;
                    continue;
                }
                const char ch = text_[i++];
                if (is_space(ch))
                    continue;
                any = true;
                if (ch == '(' || ch == '[' || ch == '{')
                    ++depth;
                else if (ch == ')' || ch == ']' || ch == '}')
                    --depth;
                else if (ch == ',' && depth == 0)
                    ++ct.raw_base_argc;
            }
            ct.raw_base_argc += !from_ast && any ? 1 : 0;
            add_slice(args, &cls, [where, index](std::string&& t) { where()[index].raw_base_args = std::move(t); });
        }
        else
        {
            CXCursor field = clang_getNullCursor();
            for (CXCursor m : children(owner))
                if (clang_getCursorKind(m) == CXCursor_FieldDecl && spelling(m) == word)
                    field = m;
            const uint32_t at = static_cast<uint32_t>(s);
            if (clang_Cursor_isNull(field))
                fail(at,
                    "'" + word + "' is not a field of the script class '" + cls.display +
                        "'; only a field or the base class can be in the initializer list");
            const CXType ft = clang_getCursorType(field);
            if (ft.kind == CXType_LValueReference || ft.kind == CXType_RValueReference)
                fail(at,
                    "the reference field '" + word +
                        "' cannot be in the initializer list: a field lives in the state; use a default member initializer");
            if (clang_isConstQualifiedType(ft))
                fail(at,
                    "the const field '" + word +
                        "' cannot be in the initializer list: a field lives in the state; use a default member initializer");
            if (ft.kind == CXType_ConstantArray || ft.kind == CXType_IncompleteArray ||
                ft.kind == CXType_VariableArray || ft.kind == CXType_DependentSizedArray)
                fail(at, "the array field '" + word + "' cannot be in the initializer list; assign its elements in the body");
            for (const Item& it : items)
                if (it.name == word)
                    fail(at, "the field '" + word + "' is initialized twice in this constructor");
            items.push_back(Item{word, text_[a] == '{', pos_of(field), args.begin, args.end});
        }
        if (q == b.begin)
            break;
        p = q + 1;
    }
    // A default member initializer runs before the list, so it cannot read a value from the list.
    for (CXCursor m : children(owner))
    {
        if (clang_getCursorKind(m) != CXCursor_FieldDecl)
            continue;
        bool listed = false;
        for (const Item& it : items)
            listed = listed || it.name == spelling(m);
        if (listed)
            continue; // C++ does not run the default of a field that has an entry
        std::function<void(CXCursor)> scan = [&](CXCursor k)
        {
            const CXCursorKind kk = clang_getCursorKind(k);
            if (kk == CXCursor_MemberRefExpr || kk == CXCursor_DeclRefExpr)
            {
                const CXCursor r = clang_getCursorReferenced(k);
                if (clang_getCursorKind(r) == CXCursor_FieldDecl &&
                    clang_equalCursors(clang_getCursorSemanticParent(r), owner))
                    for (const Item& it : items)
                        if (it.name == spelling(r))
                            fail(pos_of(k),
                                "the default member initializer of the field '" + spelling(m) + "' reads the field '" +
                                    it.name + "', which this constructor sets in its initializer list; the default sees "
                                    "the default value, not the value from the list. Add '" + spelling(m) +
                                    "' to the initializer list");
            }
            for (CXCursor c2 : children(k))
                scan(c2);
        };
        for (CXCursor k : children(m))
            if (clang_isExpression(clang_getCursorKind(k)))
                scan(k);
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.decl < y.decl; });
    ast::Ctor& ct = where()[index];
    ct.raw_pre.resize(items.size());
    for (size_t k = 0; k < items.size(); ++k)
    {
        const Item& it = items[k];
        ct.raw_pre[k].line = line(it.open);
        const std::string field = "__st->" + it.name;
        const std::string open = it.brace ? "{" : "(", close = it.brace ? "}" : ")";
        add_slice(Range{it.open, it.close}, &cls,
            [where, index, k, field, open, close](std::string&& t)
            {
                // The value is a local, so the direct initialization of C++ builds it. An empty list
                // value-initializes it. A line comment in the text needs a new line before the closing mark.
                const std::string v = trim(t);
                const std::string end = v.find("//") == std::string::npos ? close : "\n" + close;
                where()[index].raw_pre[k].text = "{ decltype(" + field + ") __hot_mi" + (v.empty() ? "{}" : open + v + end) +
                    "; " + field + " = \x01std::move(__hot_mi); }";
            });
    }
}

// Reads the body and the base initializer of the constructor "index" from the definition "c".
void Frontend::read_ctor_body(CXCursor c, const ClassInfo& cls, const Where<ast::Ctor>& where, size_t index)
{
    ast::Ctor& ct = where()[index];
    const uint32_t cpos = pos_of(c);
    const Range b = extent_of(body_of(c));
    warn_static_locals(c);
    ct.end_pos = b.end - 1;
    ct.end_line = line(ct.end_pos);
    // The text between the ")" of the parameters and the "{" of the body has the initializers.
    size_t open = text_.find('(', cpos);
    size_t close = open == std::string::npos ? open : match_close(text_, open, b.begin);
    if (close == std::string::npos)
        fail(ct.pos, "cannot find the parameter list of the constructor");
    size_t i = skip_ws(text_, close + 1, b.begin);
    // Skip the words before the ":" (for example "noexcept").
    while (i < b.begin && text_[i] != ':')
    {
        const size_t k = skip_opaque(text_, i); // a comment can hold a ":"
        i = k > i ? k : i + 1;
    }
    if (i < b.begin)
        read_member_inits(c, cls, where, index, static_cast<uint32_t>(i), b);
    ct.raw_line = line(b.begin); // "raw_body" starts right after the "{"
    add_slice(Range{b.begin + 1, b.end - 1}, &cls,
        [where, index](std::string&& s) { where()[index].raw_body = std::move(s); });
}

// The chunks before the block of a class: the consts of the static const members, the module
// block (when the first module variable is a static data member of this class) and the
// prototypes of the static methods. The text of the chunks comes later, from the slices.
void Frontend::pre_class(CXCursor c)
{
    const std::string cu = usr(c);
    const Range ce = extent_of(c);
    auto chunk_at = [&](uint32_t pos)
    {
        ast::Script::Chunk ch;
        ch.ns = ast::gen_namespace(cur_ns_); // the generated names of the class are in this namespace
        ch.pos = pos;
        ch.line = line(pos);
        sc_.chunks.push_back(std::move(ch));
        return sc_.chunks.size() - 1;
    };
    bool has_var = false;
    std::string first_var;
    for (CXCursor m : children(c))
    {
        if (clang_getCursorKind(m) != CXCursor_VarDecl)
            continue;
        const Entry* en = lookup(m);
        if (en && en->kind == Kind::ScriptStaticConst)
            static_fns_[usr(m)].proto = chunk_at(extent_of(m).begin);
        else if (en && en->kind == Kind::ScriptStatic && !has_var)
        {
            has_var = true;
            first_var = en->owner_shown() + "::" + en->name;
        }
    }
    if (has_var && first_var_pos_ == UINT32_MAX)
    { // the module block comes before the class: the methods use the static data members
        first_var_pos_ = ce.begin;
        first_var_name_ = first_var;
        const size_t i = chunk_at(ce.begin);
        sc_.chunks[i].kind = ast::Script::Chunk::Kind::Module;
    }
    for (CXCursor m : children(c))
        if (clang_getCursorKind(m) == CXCursor_CXXMethod && clang_CXXMethod_isStatic(m) &&
            !static_hook_of(m, cu, nullptr))
            static_fns_[usr(m)].proto = chunk_at(extent_of(m).begin);
}

// True when "m" is a static method "convert_<name>" and the class has the static data member
// "<name>". The method is the convert hook of the module variable then.
bool Frontend::static_hook_of(CXCursor m, const std::string& owner_usr, std::string* var) const
{
    if (clang_getCursorKind(m) != CXCursor_CXXMethod || !clang_CXXMethod_isStatic(m))
        return false;
    const std::string n = spelling(m);
    if (n.rfind("convert_", 0) != 0)
        return false;
    for (const auto& v : table_)
        if (v.second.kind == Kind::ScriptStatic && v.second.owner_usr == owner_usr &&
            v.second.name == n.substr(std::strlen("convert_")))
        {
            if (var)
                *var = v.second.name;
            return true;
        }
    return false;
}

// Records the edit that removes the word "static" before the name of the declaration "c" and
// the edit that gives the name "<Class>__s_<name>".
void Frontend::drop_static_word(CXCursor c)
{
    const Entry* en = lookup(c);
    const Range e = extent_of(c);
    const uint32_t name = pos_of(c);
    if (!en || name < e.begin || text_.compare(name, en->name.size(), en->name) != 0)
        fail(name, "cannot read the declaration of this static member");
    fixed_edits_.push_back(Edit{name, static_cast<uint32_t>(en->name.size()), en->mangled});
    for (uint32_t i = e.begin; i < name; ++i)
        if (word_at(text_, i, "static"))
        {
            uint32_t n = 6;
            while (i + n < name && (text_[i + n] == ' ' || text_[i + n] == '\t'))
                ++n;
            fixed_edits_.push_back(Edit{i, n, std::string()});
            return;
        }
}

// A static const or constexpr data member is a const at file scope before the class block. The
// text is the declaration without "static", with the mangled name.
void Frontend::make_static_const(CXCursor c, const ClassInfo* cls)
{
    bool init = false;
    for (CXCursor k : children(c))
        init = init || clang_isExpression(clang_getCursorKind(k));
    if (!init)
        fail(pos_of(c), "a static const member needs an initializer in the class");
    // The const gets its value when the library loads. The module state does not exist then.
    // The prototype of a static method comes after the const.
    check_static_init(c);
    std::string used;
    uint32_t used_pos = 0;
    if (find_module_ref(c, nullptr, used, used_pos, kFns))
        fail(used_pos,
            "the static const member '" + spelling(c) + "' cannot call the static method '" + used +
                "': it gets its value before the prototype of the method");
    drop_static_word(c);
    const size_t idx = static_fns_.at(usr(c)).proto;
    add_slice(extent_of(c), cls, [this, idx](std::string&& s) { sc_.chunks[idx].text = s + ";"; });
}

// A static method is a free function "<Class>__s_<name>". The prototype comes before the class
// block, the definition after it. The prototype has the default arguments and the definition does
// not (C++ allows a default argument only once). So the text up to the body is in pieces: the
// text between the defaults, and the defaults. The definition keeps the lines of the defaults.
void Frontend::make_static_fn(CXCursor c, const ClassInfo* cls)
{
    const Range b = extent_of(body_of(c));
    warn_static_locals(c);
    drop_static_word(c);
    const Range e = extent_of(c);
    ast::Script::Chunk def;
    def.ns = ast::gen_namespace(cur_ns_);
    def.pos = e.begin;
    def.line = line(e.begin);
    sc_.chunks.push_back(std::move(def));
    StaticFn& fn = static_fns_.at(usr(c));
    fn.def = sc_.chunks.size() - 1;
    const size_t proto = fn.proto, defn = fn.def;
    std::vector<Range> pieces; // even index: text of the header, odd index: a default argument
    uint32_t at = e.begin;
    const int n = clang_Cursor_getNumArguments(c);
    for (int i = 0; i < n; ++i)
        for (CXCursor k : children(clang_Cursor_getArgument(c, static_cast<unsigned>(i))))
            if (clang_isExpression(clang_getCursorKind(k)))
            {
                // An array bound or a template argument of the type is an expression too. The
                // default argument follows a "=" (or it is a "{...}" after the name).
                Range d = extent_of(k);
                // The extent of a class type default with a braced list ("= {}") starts at the "=".
                if (d.begin < text_.size() && text_[d.begin] == '=' && d.begin + 1 < d.end)
                {
                    ++d.begin;
                    while (d.begin < d.end && is_space(text_[d.begin]))
                        ++d.begin;
                }
                size_t q = d.begin;
                while (q > 0 && is_space(text_[q - 1]))
                    --q;
                const bool after_eq = q > 0 && text_[q - 1] == '=' && !(q > 1 && std::strchr("=!<>", text_[q - 2]));
                const bool brace = d.begin < text_.size() && text_[d.begin] == '{' && q > 0 && is_ident(text_[q - 1]);
                if ((after_eq || brace) && d.begin >= at && d.end > d.begin && d.end <= b.begin)
                {
                    pieces.push_back(Range{at, d.begin});
                    pieces.push_back(d);
                    at = d.end;
                    break;
                }
            }
    pieces.push_back(Range{at, b.begin});
    auto parts = std::make_shared<std::vector<std::string>>(pieces.size());
    for (size_t i = 0; i < pieces.size(); ++i)
        add_slice(pieces[i], cls, [parts, i](std::string&& s) { (*parts)[i] = std::move(s); });
    add_slice(b, cls,
        [this, parts, proto, defn](std::string&& body)
        {
            std::string all, header;
            for (size_t i = 0; i < parts->size(); ++i)
            {
                const std::string& p = (*parts)[i];
                all += p;
                if (i % 2 == 1)
                { // a default argument: the definition keeps only its lines
                    header.append(static_cast<size_t>(std::count(p.begin(), p.end(), '\n')), '\n');
                    continue;
                }
                std::string t = p;
                if (i + 1 < parts->size())
                { // remove the "=" before the default argument
                    const size_t keep = t.find_last_not_of(" \t\r\n");
                    if (keep != std::string::npos && t[keep] == '=')
                    {
                        const std::string cut = t.substr(keep);
                        t.resize(keep);
                        const size_t k2 = t.find_last_not_of(" \t");
                        t.resize(k2 == std::string::npos ? 0 : k2 + 1);
                        header += t;
                        header.append(static_cast<size_t>(std::count(cut.begin(), cut.end(), '\n')), '\n');
                        continue;
                    }
                }
                header += t;
            }
            sc_.chunks[proto].text = trim(all) + ";";
            sc_.chunks[defn].text = header + body;
        });
}

// The definition outside the class of a static data member: "T Player::x = init;". The text
// gives the initializer of the module variable. It does not stay in the output.
void Frontend::read_static_def(CXCursor c)
{
    const Entry* en = lookup(c);
    if (en->kind == Kind::ScriptStaticConst)
        return; // the class has the value: a static const member is a const before the class block
    auto it = static_vars_.find(usr(c));
    if (it == static_vars_.end())
        fail(pos_of(c), "internal error: a static data member has no module variable");
    check_late_refs(c);
    read_init(c, nullptr, [this]() -> std::vector<ast::Field>& { return sc_.vars; }, it->second);
}

void Frontend::process_class(CXCursor c)
{
    const size_t ci = sc_.classes.size(); // the slices find the class by this index
    sc_.classes.emplace_back();
    ast::ClassDef& cd = sc_.classes.back(); // valid until the next class
    const Entry* self = lookup(c);
    if (!self)
        fail(pos_of(c), "internal error: the script class has no table entry");
    cd.name = self->name; // "game__Player" for "game::Player"
    cd.ns = ns_of(c);
    if (!cd.ns.empty())
        cd.key = self->shown();
    cd.pos = pos_of(c);
    cd.line = line(cd.pos);
    infos_.emplace_back();
    ClassInfo& info = infos_.back();
    info.name = cd.name;
    info.simple = spelling(c);
    info.display = self->shown();
    info.usr = usr(c);
    info.end = extent_of(c).end;
    class_infos_[info.usr] = &info; // a deque does not move its elements
    // HOT_ATTRIBUTE("Name") on the class is a script attribute tag, the same as "[Name]" in a .hot
    // script. The translator checks that a prelude declares the attribute class.
    for (CXCursor k : children(c))
        if (clang_getCursorKind(k) == CXCursor_AnnotateAttr)
        {
            const std::string a = spelling(k);
            if (a.rfind("hot.alias=", 0) == 0) // a class has no previous name: the record key is the name
                fail(pos_of(c), "'HOT_ALIAS' on the class '" + spelling(c) +
                        "' is not supported in a .cpp script; use it on a field or a module variable");
            if (a.rfind("hot.attribute=", 0) == 0)
            {
                std::string name = a.substr(std::strlen("hot.attribute="));
                if (std::find(cd.attributes.begin(), cd.attributes.end(), name) != cd.attributes.end())
                    fail(pos_of(c), "duplicate attribute '" + name + "'");
                cd.attributes.push_back(std::move(name));
            }
        }
    if (auto si = shell_info_.find(pos_of(c)); si != shell_info_.end())
    {
        cd.cpp_prelude = si->second.prelude;
        cd.cpp_overrides = si->second.overrides;
    }
    // A child of a script class (the same design as a .hot child): the shell of the child derives
    // from the shell of the parent, and the state of the child holds the fields of the chain.
    const ClassInfo* parent = nullptr;
    size_t n_bases = 0;
    for (CXCursor k : children(c))
        n_bases += clang_getCursorKind(k) == CXCursor_CXXBaseSpecifier;
    for (CXCursor k : children(c))
    {
        if (clang_getCursorKind(k) != CXCursor_CXXBaseSpecifier)
            continue;
        CXCursor decl = clang_getTypeDeclaration(clang_getCursorType(k));
        const Entry* sb = class_entry(clang_getCursorType(k));
        if (sb)
        {
            const std::string what = "the script class '" + sb->shown() + "'";
            if (n_bases > 1)
                fail(pos_of(k), "a .cpp script class with the base " + what + " cannot have another base class");
            if (clang_isVirtualBase(k))
                fail(pos_of(k), "a .cpp script class cannot derive from " + what + " with 'virtual'");
            if (clang_getCXXAccessSpecifier(k) != CX_CXXPublic)
                fail(pos_of(k), "a .cpp script class must derive from " + what + " with 'public'");
            auto it = class_infos_.find(sb->owner_usr);
            if (it == class_infos_.end())
                fail(pos_of(k), "internal error: the parent class '" + sb->shown() + "' has no record");
            parent = it->second;
        }
        else if (!clang_Cursor_isNull(decl) && from_main(decl))
            fail(pos_of(k),
                "a .cpp script class cannot derive from '" + spelling(decl) +
                    "': it is a plain class of the script; derive from a script class or from a native class");
        else if (!clang_Cursor_isNull(decl))
        {
            CXFile f = nullptr;
            clang_getExpansionLocation(clang_getCursorLocation(decl), &f, nullptr, nullptr, nullptr);
            const std::string fn = f ? cxs(clang_getFileName(f)) : std::string();
            if (fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".cpp") == 0)
                fail(pos_of(k),
                    "a .cpp script class cannot derive from '" + spelling(decl) +
                        "' of another .cpp file; a script class can derive only from a script class of the same file");
        }
        std::string b = sb ? sb->name : type_text(clang_getCursorType(k));
        if (cd.base.empty())
            cd.base = b;
        else
            cd.interfaces.push_back(b);
    }
    info.base = cd.base;
    info.base_simple = parent ? parent->simple : last_name(cd.base);
    if (parent)
    {
        info.native = parent->native;
        info.base_usrs = parent->base_usrs;
        info.script_bases = parent->script_bases;
        info.script_bases.insert(parent->usr);
    }
    else
    {
        info.native = cd.base;
        collect_bases(c, info.base_usrs);
    }
    pre_class(c);
    const Range ce = extent_of(c);
    ast::Script::Chunk block;
    block.kind = ast::Script::Chunk::Kind::Class;
    block.pos = ce.begin;
    block.line = line(ce.begin);
    block.name = cd.name;
    sc_.chunks.push_back(std::move(block));
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
        {
            const Entry* en = lookup(k);
            if (en && en->kind == Kind::ScriptStaticConst)
            {
                make_static_const(k, &info);
                break;
            }
            check_late_refs(k);
            make_field(k, &info, [this]() -> std::vector<ast::Field>& { return sc_.vars; }, en->mangled);
            static_vars_[usr(k)] = sc_.vars.size() - 1;
            break;
        }
        case CXCursor_CXXMethod:
        {
            std::string var;
            if (clang_CXXMethod_isStatic(k))
            { // make_method reports an operator; a static function never reaches it
                const std::string sp = spelling(k);
                if (sp.compare(0, 8, "operator") == 0 && (sp.size() == 8 || !is_ident(sp[8])) && !is_defaulted(k))
                    fail(pos_of(k), "a static operator is not supported in a .cpp script class");
            }
            if (!clang_CXXMethod_isStatic(k))
                make_method(k, &info, [this, ci]() -> std::vector<ast::Method>& { return sc_.classes[ci].methods; });
            else if (static_hook_of(k, info.usr, &var))
            { // a convert hook of the module variable "<Class>__s_<var>"
                check_hook_refs(k);
                make_method(k, &info, [this]() -> std::vector<ast::Method>& { return sc_.var_hooks; });
                sc_.var_hooks.back().name = "convert_" + info.name + "__s_" + var;
            }
            else
                make_static_fn(k, &info);
            break;
        }
        case CXCursor_Constructor:
            make_ctor(k, info, [this, ci]() -> std::vector<ast::Ctor>& { return sc_.classes[ci].ctors; });
            break;
        case CXCursor_Destructor:
            make_method(k, &info, [this, ci]() -> std::vector<ast::Method>& { return sc_.classes[ci].dtors; });
            break;
        default:
            fail(pos_of(k), unsupported_member(kind) + " is not supported in a .cpp script class");
        }
    }
    // A class with a pure method that no method overrides is abstract. A child that does not
    // override an inherited pure method stays abstract. A pure method of a native base that no
    // class of the chain overrides also makes the class abstract: libclang knows the whole chain.
    std::set<std::string> open;
    if (parent)
        open = open_pure_[parent->usr];
    for (CXCursor k : children(c))
    {
        if (clang_getCursorKind(k) != CXCursor_CXXMethod)
            continue;
        CXCursor* over = nullptr;
        unsigned n = 0;
        clang_getOverriddenCursors(k, &over, &n);
        for (unsigned i = 0; i < n; ++i)
            open.erase(usr(over[i]));
        clang_disposeOverriddenCursors(over);
        if (pure_usrs_.count(usr(k)))
            open.insert(usr(k));
    }
    sc_.classes[ci].is_abstract = !open.empty() || clang_CXXRecord_isAbstract(c);
    open_pure_[info.usr] = std::move(open);
}

// A file-scope declaration of the main file. The text stays as written. With "edit", a slice
// over the extent gives the edits of a body, so the module variables become hot_mod() reads.
// A ";" after the extent belongs to the declaration.
void Frontend::add_text(Range e, bool edit)
{
    ast::Script::Chunk ch;
    ch.ns = cur_ns_;
    ch.pos = e.begin;
    ch.line = line(e.begin);
    ch.text = text_.substr(e.begin, e.end - e.begin);
    std::string semi;
    size_t i = skip_ws(text_, e.end, text_.size());
    if (i < text_.size() && text_[i] == ';')
        semi = ";";
    ch.text += semi;
    const size_t index = sc_.chunks.size();
    sc_.chunks.push_back(std::move(ch));
    if (edit)
        add_slice(e, nullptr, [this, index, semi](std::string&& s) { sc_.chunks[index].text = s + semi; });
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
    ast::Script::Chunk ch; // the directive stays in its place in the output
    ch.pos = im.pos;
    ch.line = im.line;
    ch.text = "#include " + im.target();
    sc_.chunks.push_back(std::move(ch));
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

// Records a member of the script class "owner_usr". An out-of-class method definition has the
// USR of its declaration: the first entry stays. The same is true for a static data member.
void Frontend::index_member(CXCursor k, const std::string& owner_usr, const std::string& owner_name,
    const std::string& owner_display, uint32_t cpos)
{
    const CXCursorKind kind = clang_getCursorKind(k);
    Entry en{Kind::ScriptField, owner_usr, owner_name, spelling(k), std::string()};
    en.cpos = cpos;
    en.owner_display = owner_display;
    if (kind == CXCursor_VarDecl)
    {
        en.kind = clang_isConstQualifiedType(clang_getCursorType(k)) ? Kind::ScriptStaticConst : Kind::ScriptStatic;
        en.mangled = owner_name + "__s_" + en.name;
    }
    else if (kind == CXCursor_CXXMethod)
    {
        const bool is_static = clang_CXXMethod_isStatic(k) != 0;
        en.kind = is_static ? Kind::ScriptStaticFn : Kind::ScriptMethod;
        if (is_static)
            en.mangled = owner_name + "__s_" + en.name;
        en.virt = clang_CXXMethod_isVirtual(k) || has_child(k, CXCursor_CXXOverrideAttr);
        if (!is_static)
            en.vowner = slot_owner(k);
    }
    else if (kind != CXCursor_FieldDecl)
        return;
    table_.emplace(usr(k), std::move(en));
}

// Two script classes, or two module variables, must not get the same file scope name. The class
// "game::Player" and the class "game__Player" both give "game__Player".
void Frontend::check_unique_name(CXCursor k, const Entry& en)
{
    for (const auto& v : table_)
        if (v.second.kind == en.kind && v.second.name == en.name && v.second.display != en.display)
            fail(pos_of(k),
                "'" + en.display + "' and '" + v.second.display + "' have the same generated name '" + en.name
                    + "'. Rename one of them");
}

// A static member "x" has the generated name "<Class>__s_x". An instance method "s_x" has the same
// name. The two must not be in one class.
void Frontend::check_static_names(CXCursor cls, const std::string& cls_name, const std::string& display)
{
    std::map<std::string, CXCursor> methods; // the instance methods
    for (CXCursor m : children(cls))
        if (clang_getCursorKind(m) == CXCursor_CXXMethod && !clang_CXXMethod_isStatic(m))
            methods.emplace(spelling(m), m);
    for (CXCursor m : children(cls))
    {
        const CXCursorKind kind = clang_getCursorKind(m);
        if (kind != CXCursor_VarDecl && !(kind == CXCursor_CXXMethod && clang_CXXMethod_isStatic(m)))
            continue;
        auto it = methods.find("s_" + spelling(m));
        if (it != methods.end())
            fail(pos_of(it->second),
                "the method '" + display + "::" + it->first + "' and the static member '" + display + "::"
                    + spelling(m) + "' have the same generated name '" + cls_name + "__s_" + spelling(m)
                    + "'. Rename one of them");
    }
}

// Index the declarations of the main file: the script classes with their members and the module
// variables. The second loop is for out-of-class method definitions. It adds nothing yet.
void Frontend::index(const std::vector<CXCursor>& top)
{
    for (CXCursor k : top)
    {
        if (is_script_class(k))
        {
            // A class in a namespace has the file-scope name "game__Player". The class record has
            // the name "game::Player". The two classes "a::X" and "b::X" have two different names.
            const std::string u = usr(k), ns = ns_of(k), n = mangle_in(ns, spelling(k));
            Entry cls{Kind::ScriptClass, u, n, n, std::string()};
            cls.display = ns.empty() ? n : ns + "::" + spelling(k);
            check_unique_name(k, cls);
            table_[u] = cls;
            // A child class needs the table entry of its parent: the parent comes first in the text.
            const uint32_t cpos = extent_of(k).begin;
            for (CXCursor m : children(k))
                index_member(m, u, n, cls.display, cpos);
            check_static_names(k, n, cls.display);
        }
        else if (is_module_var(k) && !is_record(clang_getCursorKind(clang_getCursorSemanticParent(k))))
        { // a variable in a namespace is the member "game__score" of the module block
            const std::string ns = ns_of(k);
            Entry var{Kind::ModuleVar, std::string(), std::string(), mangle_in(ns, spelling(k)), std::string()};
            var.display = ns.empty() ? var.name : ns + "::" + spelling(k);
            check_unique_name(k, var);
            table_[usr(k)] = var;
        }
    }
    for (CXCursor k : top)
    {
        if (clang_getCursorKind(k) != CXCursor_CXXMethod)
            continue;
        auto it = table_.find(usr(clang_getCursorSemanticParent(k)));
        if (it != table_.end() && it->second.kind == Kind::ScriptClass)
            index_member(k, it->first, it->second.name, it->second.display, 0);
    }
}

// True when the namespace "ns" holds a script class, a module variable, or the definition of a
// member of a class outside the class. The text of such a namespace cannot stay as one block.
bool Frontend::holds_script(CXCursor ns) const
{
    for (CXCursor c : children(ns))
    {
        if (!from_main(c))
            continue;
        const CXCursorKind kind = clang_getCursorKind(c);
        if (kind == CXCursor_Namespace)
        {
            if (holds_script(c))
                return true;
            continue;
        }
        if (is_script_class(c) ||
            (is_module_var(c) && !is_record(clang_getCursorKind(clang_getCursorSemanticParent(c)))))
            return true;
        const bool member_kind = kind == CXCursor_CXXMethod || kind == CXCursor_Constructor ||
            kind == CXCursor_Destructor || kind == CXCursor_VarDecl;
        if (member_kind && clang_isCursorDefinition(c) &&
            is_script_class(clang_getCursorSemanticParent(c)))
            return true;
    }
    return false;
}

// Adds a top-level cursor of the main file. A namespace with script code gives its declarations,
// each with its namespace. Other namespaces stay one declaration with the text as written.
void Frontend::add_top(CXCursor k)
{
    if (clang_getCursorKind(k) == CXCursor_Namespace && holds_script(k))
    {
        if (spelling(k).empty())
            fail(pos_of(k),
                "an anonymous namespace cannot hold a script class or a module variable in a .cpp script; "
                "use a named namespace");
        if (clang_Cursor_isInlineNamespace(k))
            fail(pos_of(k),
                "an inline namespace cannot hold a script class or a module variable in a .cpp script; "
                "remove 'inline'");
        for (CXCursor c : children(k))
            if (from_main(c))
                add_top(c);
        return;
    }
    top_.push_back(k);
}

const Frontend::Entry* Frontend::lookup(CXCursor ref) const
{
    auto it = table_.find(usr(ref));
    return it == table_.end() ? nullptr : &it->second;
}

// The header of a script class can name shell macros: "HOT_SHELL_MACRO("URHO3D_OBJECT")" in
// the class annotations. A call of such a macro in the body of the class declares members for
// the shell, not for the script. This pass finds the calls. The shell gets the text of the call,
// and the walks of the class do not see its members. "HOT_NO_SLOT("A B")" names the virtuals that
// the shell keeps without a slot. The .cpp file stays a normal C++ file: the macro is a C++ macro.
void Frontend::find_shell_macros()
{
    const std::string shell_key = "hot.shell_macro=", slot_key = "hot.no_slot=";
    for (CXCursor c : top_)
    {
        const CXCursorKind ck = clang_getCursorKind(c);
        if ((ck != CXCursor_ClassDecl && ck != CXCursor_StructDecl) || !clang_isCursorDefinition(c))
            continue;
        std::set<std::string> names;
        std::string overrides;
        for (CXCursor k : children(c))
        {
            if (clang_getCursorKind(k) != CXCursor_AnnotateAttr)
                continue;
            const std::string a = spelling(k);
            if (a.rfind(shell_key, 0) == 0)
                names.insert(a.substr(shell_key.size()));
            else if (a.rfind(slot_key, 0) == 0)
                overrides += (overrides.empty() ? "" : " ") + a.substr(slot_key.size());
        }
        if (names.empty() && overrides.empty())
            continue;
        ShellInfo info;
        info.overrides = overrides;
        const Range ce = extent_of(c);
        for (CXCursor m : top_)
        {
            if (clang_getCursorKind(m) != CXCursor_MacroExpansion || !names.count(spelling(m)))
                continue;
            const uint32_t begin = extent_of(m).begin;
            if (!ce.has(begin))
                continue;
            // The call ends at the ")" that matches the "(" after the name.
            size_t i = skip_ws(text_, begin + spelling(m).size(), text_.size());
            size_t end = begin + spelling(m).size();
            if (i < text_.size() && text_[i] == '(')
            {
                int depth = 0;
                for (; i < text_.size(); ++i)
                {
                    depth += text_[i] == '(';
                    depth -= text_[i] == ')';
                    if (depth == 0)
                        break;
                }
                end = i < text_.size() ? i + 1 : text_.size();
            }
            hidden_.ranges.emplace_back(begin, static_cast<uint32_t>(end));
            info.prelude += text_.substr(begin, end - begin) + ";\n";
        }
        shell_info_[pos_of(c)] = std::move(info);
    }
}

ast::Script Frontend::run(CXTranslationUnit tu, const std::string& name)
{
    main_ = clang_getFile(tu, name.c_str());
    for (CXCursor k : children(clang_getTranslationUnitCursor(tu)))
        if (from_main(k))
            add_top(k);
    hidden_.file = main_;
    find_shell_macros();
    struct HiddenScope
    {
        explicit HiddenScope(HiddenCalls* h) { g_hidden = h; }
        ~HiddenScope() { g_hidden = nullptr; }
    } hidden_scope(&hidden_);
    index(top_);
    try
    {
        read_top(top_);
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
        cur_ns_ = ns_of(k);
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
        if ((kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) && !en)
        { // the generated code names the class: a forward declaration of it does not fit
            const Entry* fwd = lookup(k);
            if (fwd && fwd->kind == Kind::ScriptClass)
                fail(pos_of(k),
                    "a forward declaration of the script class '" + fwd->shown() +
                        "' is not supported in a .cpp script; define the class before its first use");
        }
        check_type_refs(k);
        const Range e = extent_of(k);
        if (is_member_def(k) && clang_isCursorDefinition(k))
            define_member(k); // the body goes to the member in the class; the text does not stay
        else if (script_class)
            process_class(k);
        else if (kind == CXCursor_VarDecl && en && (en->kind == Kind::ScriptStatic || en->kind == Kind::ScriptStaticConst))
            read_static_def(k);
        else if (en && en->kind == Kind::ModuleVar)
        {
            if (first_var_pos_ == UINT32_MAX)
            { // the module block comes at the place of the first module variable
                first_var_pos_ = e.begin;
                first_var_name_ = spelling(k);
                ast::Script::Chunk ch;
                ch.kind = ast::Script::Chunk::Kind::Module;
                ch.pos = e.begin;
                ch.line = line(e.begin);
                sc_.chunks.push_back(std::move(ch));
            }
            check_late_refs(k);
            make_field(k, nullptr, [this]() -> std::vector<ast::Field>& { return sc_.vars; }, en->name);
        }
        else if (kind == CXCursor_FunctionDecl && clang_isCursorDefinition(k))
        {
            const std::string n = spelling(k);
            bool hook = false;
            if (n.rfind("convert_", 0) == 0)
                for (const auto& v : table_)
                {
                    const bool same = v.second.kind == Kind::ModuleVar && v.second.name == n.substr(std::strlen("convert_"));
                    hook = hook || (same && cur_ns_.empty());
                    // A hook has the file-scope name of the variable: "convert_game__score". The
                    // same name in a namespace, or the short name in the namespace of the variable,
                    // is an error.
                    if (v.second.kind == Kind::ModuleVar && !cur_ns_.empty() &&
                        (same || v.second.shown() == cur_ns_ + "::" + n.substr(std::strlen("convert_"))))
                        fail(pos_of(k),
                            "the convert hook of the module variable '" + v.second.shown() +
                                "' must be at file scope and must be named 'convert_" + v.second.name + "'");
                }
            std::string var;
            uint32_t var_pos = 0;
            if (hook)
            {
                check_hook_refs(k);
                make_method(k, nullptr, [this]() -> std::vector<ast::Method>& { return sc_.var_hooks; });
            }
            else
            {
                warn_static_locals(k);
                add_text(e, find_module_ref(k, nullptr, var, var_pos, kAll) || names_script_class(k));
            }
        }
        else if (kind == CXCursor_MacroDefinition)
        { // the extent starts at the name: take the "#define" of the line too
            size_t b = e.begin;
            while (b > 0 && text_[b - 1] != '\n')
                --b;
            add_text(Range{static_cast<uint32_t>(b), e.end}, false);
        }
        else
        { // a template, a namespace, a prototype, a plain type: the edits of a body apply to all of it
            warn_plain_text(k);
            check_static_init(k);
            std::string var;
            uint32_t var_pos = 0;
            add_text(e, find_module_ref(k, nullptr, var, var_pos, kAll) || names_script_class(k));
        }
    }
    check_all_defined();
}

} // namespace

ast::Script parse_cpp_script(const std::string& text, const std::string& name,
    const std::vector<std::string>& clang_args, const LineMap& map, Diagnostics& diag,
    const std::vector<std::pair<std::string, std::string>>& virtual_files, bool aot_optimized)
{
    ast::Script sc;
    std::vector<const char*> cargs;
    for (const auto& a : clang_args)
        cargs.push_back(a.c_str());
    Handles h;
    h.idx = clang_createIndex(0, 0);
    std::vector<CXUnsavedFile> unsaved;
    unsaved.push_back({name.c_str(), text.c_str(), static_cast<unsigned long>(text.size())});
    // Each name that the include search of libclang can make (hotc::clang_spellings). A deque
    // keeps the names at the same address.
    std::deque<std::string> unsaved_names;
    for (const auto& [vpath, vtext] : virtual_files)
        for (std::string& spelling : hotc::clang_spellings(vpath))
        {
            unsaved_names.push_back(std::move(spelling));
            unsaved.push_back({unsaved_names.back().c_str(), vtext.c_str(), static_cast<unsigned long>(vtext.size())});
        }
    CXErrorCode ec = clang_parseTranslationUnit2(h.idx, name.c_str(), cargs.data(), static_cast<int>(cargs.size()),
        unsaved.data(), static_cast<unsigned>(unsaved.size()), CXTranslationUnit_DetailedPreprocessingRecord, &h.tu);
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
            {
                std::string msg = cxs(clang_getDiagnosticSpelling(d));
                // The same text as the .hot error for "new" of an abstract class.
                static const std::string abstract_new = "allocating an object of abstract class type ";
                if (msg.compare(0, abstract_new.size(), abstract_new) == 0)
                    msg = "cannot create an instance of the abstract class " + msg.substr(abstract_new.size());
                diag.add(offset_of(loc), map, msg);
                // The notes of the error. Notes in a header are dropped. libclang owns the child
                // set, but the caller must dispose each diagnostic that clang_getDiagnosticInSet returns.
                CXDiagnosticSet children = clang_getChildDiagnostics(d);
                for (unsigned c = 0, cn = clang_getNumDiagnosticsInSet(children); c < cn; ++c)
                {
                    CXDiagnostic child = clang_getDiagnosticInSet(children, c);
                    CXSourceLocation cloc = clang_getDiagnosticLocation(child);
                    if (clang_Location_isFromMainFile(cloc))
                        diag.note(offset_of(cloc), map, cxs(clang_getDiagnosticSpelling(child)));
                    clang_disposeDiagnostic(child);
                }
            }
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
        Frontend fe(text, map, aot_optimized);
        sc = fe.run(h.tu, name);
        for (const auto& [wpos, wmsg] : fe.warnings())
            diag.warn(wpos, map, wmsg);
    }
    catch (const CppFail& f)
    {
        diag.add(f.pos, map, f.msg);
        return ast::Script{};
    }
    sc.is_cpp = true;
    return sc;
}

} // namespace hotc
