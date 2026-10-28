// sppc_lib: the translator from script text to the text of a generation .cpp file.
//
// The translator has three parts.
//   1. A parser (src/translator/parser/). It reads the structure of the script:
//      imports, classes, fields, overrides and functions. It also parses each body and
//      each field initializer into an AST (ast.h). A lowering pass (lower.cpp)
//      changes the AST into C++ names.
//   2. A query to libclang (C API). It reads the virtual methods of the native
//      base class. The translator resolves script names only. Clang resolves
//      all native names when it compiles the generated file.
//   3. An emitter. It writes the shell, the state struct, the reflection table,
//      the generation functions, the thunks and the record. The printer
//      (emit.cpp) writes each body. A "#line" directive before each line makes
//      the Clang diagnostics point to the script.
//
// The function sppc::translate takes strings and returns strings. It does not
// write a file. libclang reads the imported headers from the include dirs.
// The command line tool sppc (src/translator/sppc.cpp) is a thin file wrapper.
//
// The slot file keeps the slot order of the script functions stable between the
// generations. Native virtuals come first, in the order of the base class.
// The script functions follow. A function keeps its slot in all later generations.
#include "spp/sppc_lib.h"

#include "ast.h"
#include "builtin_types.h"
#include "check.h"
#include "diag.h"
#include "emit.h"
#include "lower.h"
#include "parser/parser.h"
#include "types.h"

#include <clang-c/Index.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace sppc::ast;

// State of the call in progress. Each thread has its own copy.
thread_local std::string g_script;  // the script name, as given by the caller
thread_local sppc::Diagnostics g_diag;  // the diagnostics of the call

struct TranslateAbort {};

[[noreturn]] void fail(int line, const std::string& msg)
{
    g_diag.add_line(line, msg);
    throw TranslateAbort{};
}

// An error at a byte offset of the script. The diagnostic has a line and a column.
[[noreturn]] void fail_at(const sppc::LineMap& map, uint32_t pos, const std::string& msg)
{
    g_diag.add(pos, map, msg);
    throw TranslateAbort{};
}

// Parses the script: the structure, the method bodies and the field initializers.
// The parser reports all syntax errors in source order. Any error stops the translation.
Script parse_structure(const std::string& text, const sppc::LineMap& map)
{
    Script sc = sppc::parse_script(text, map, g_diag);
    if (!g_diag.items.empty()) throw TranslateAbort{};
    return sc;
}

// ----------------------------------------------------------------- libclang
std::string cxs(CXString s)
{
    std::string r = clang_getCString(s) ? clang_getCString(s) : "";
    clang_disposeString(s);
    return r;
}

template <class F>
void visit(CXCursor c, F&& f)
{
    auto tramp = [](CXCursor cur, CXCursor, CXClientData d) -> CXChildVisitResult {
        return (*static_cast<std::remove_reference_t<F>*>(d))(cur);
    };
    clang_visitChildren(c, tramp, &f);
}

// Removes the space that libclang puts before '&' and '*'.
std::string tidy(std::string s)
{
    for (const char* q : {" &", " *"}) {
        for (size_t p; (p = s.find(q)) != std::string::npos;) s.erase(p, 1);
    }
    return s;
}

std::string norm(const std::string& s)
{
    std::string r;
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) r += c;
    return r;
}

// The key form of a type: the spelling is canonical (see builtin_types.h) and has no space.
// A native "int" and a script "int" or "int32" give the same key.
std::string key_norm(const std::string& s) { return norm(sppc::canonical_type(s, nullptr, true)); }

struct NativeVirtual {
    std::string name, ret, owner;
    std::vector<std::string> ptypes;
    bool is_const = false, pure = false, final_ = false, no_except = false;
    std::string key() const
    {
        std::string k = name + "(";
        for (const auto& p : ptypes) k += key_norm(p) + ",";
        return k + ")" + (is_const ? "const" : "");
    }
    std::string text() const
    {
        std::string k = name + "(";
        for (size_t i = 0; i < ptypes.size(); ++i) k += (i ? ", " : "") + ptypes[i];
        return k + ")";
    }
};

struct BaseInfo {
    std::string name;
    std::vector<NativeVirtual> virtuals;
    bool virtual_dtor = false;
    int ctor_kind = 0;  // the constructor argument: 0 none, 1 std::string, 2 const char*
    bool default_ctor = false;
    bool any_ctor = false;  // the class declares a constructor
};

void collect_virtuals(CXCursor cls, BaseInfo& info)
{
    std::string owner = cxs(clang_getCursorSpelling(cls));
    visit(cls, [&](CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        if (k == CXCursor_CXXBaseSpecifier) {
            CXCursor def = clang_getCursorDefinition(clang_getCursorReferenced(c));
            if (!clang_Cursor_isNull(def) && clang_getCursorKind(def) != CXCursor_NoDeclFound)
                collect_virtuals(def, info);
        } else if (k == CXCursor_Destructor) {
            if (clang_CXXMethod_isVirtual(c)) info.virtual_dtor = true;
        } else if (k == CXCursor_CXXMethod && clang_CXXMethod_isVirtual(c) &&
                   clang_getCXXAccessSpecifier(c) != CX_CXXPrivate) {
            NativeVirtual v;
            v.name = cxs(clang_getCursorSpelling(c));
            v.owner = owner;
            v.ret = tidy(cxs(clang_getTypeSpelling(clang_getResultType(clang_getCursorType(c)))));
            int n = clang_Cursor_getNumArguments(c);
            for (int i = 0; i < n; ++i)
                v.ptypes.push_back(tidy(
                    cxs(clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(c, i))))));
            v.is_const = clang_CXXMethod_isConst(c);
            v.pure = clang_CXXMethod_isPureVirtual(c);
            auto es = clang_getCursorExceptionSpecificationType(c);
            v.no_except = es == CXCursor_ExceptionSpecificationKind_BasicNoexcept ||
                          es == CXCursor_ExceptionSpecificationKind_DynamicNone ||
                          es == CXCursor_ExceptionSpecificationKind_NoThrow;
            visit(c, [&](CXCursor a) {
                if (clang_getCursorKind(a) == CXCursor_CXXFinalAttr) v.final_ = true;
                return CXChildVisit_Continue;
            });
            auto it = std::find_if(info.virtuals.begin(), info.virtuals.end(),
                                   [&](const NativeVirtual& o) { return o.key() == v.key(); });
            if (it != info.virtuals.end())
                *it = v;  // an override in a derived native class: same position
            else
                info.virtuals.push_back(v);
        }
        return CXChildVisit_Continue;
    });
}

void collect_ctors(CXCursor cls, BaseInfo& info)
{
    visit(cls, [&](CXCursor c) {
        if (clang_getCursorKind(c) == CXCursor_Constructor &&
            clang_getCXXAccessSpecifier(c) != CX_CXXPrivate) {
            int n = clang_Cursor_getNumArguments(c);
            info.any_ctor = true;
            if (clang_CXXConstructor_isDefaultConstructor(c)) info.default_ctor = true;
            if (n == 1 && info.ctor_kind == 0) {
                std::string t = norm(
                    cxs(clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(c, 0)))));
                if (t.find("string") != std::string::npos && t.find("string_view") == std::string::npos)
                    info.ctor_kind = 1;
                else if (t == "constchar*")
                    info.ctor_kind = 2;
            }
        }
        return CXChildVisit_Continue;
    });
}

CXCursor find_class(CXCursor root, const std::string& qname)
{
    std::string name = qname.substr(qname.rfind(':') == std::string::npos ? 0 : qname.rfind(':') + 1);
    CXCursor found = clang_getNullCursor();
    visit(root, [&](CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        if ((k == CXCursor_ClassDecl || k == CXCursor_StructDecl) && clang_isCursorDefinition(c) &&
            cxs(clang_getCursorSpelling(c)) == name) {
            found = c;
            return CXChildVisit_Break;
        }
        if (k == CXCursor_Namespace) return CXChildVisit_Recurse;
        return CXChildVisit_Continue;
    });
    return found;
}

// What the headers say about a type. "known" is false when libclang cannot name the type.
// The facts come from C++ traits that the probe file evaluates (see facts_text).
struct TypeFacts {
    bool known = false;
    bool is_class = false, copy = false, nothrow_move = false, polymorphic = false, final_ = false;
    bool plain = false;  // trivially copyable and standard layout
    bool arrow = false;  // a pointer, or a type with operator->
    std::map<std::string, bool> members;  // for each asked name: "&T::name" is valid
};
using FactMap = std::map<std::string, TypeFacts>;

// The question for one type: the type text and the member names to look for.
struct FactReq {
    std::string type;
    std::vector<std::string> members;
};

// The direct bases of each native class, by qualified name, in declaration order.
using Hierarchy = std::map<std::string, std::vector<std::string>>;

// The qualified name of a declaration: the namespaces and the classes, joined with "::".
// An empty spelling (an anonymous namespace) is skipped.
std::string qualified_name(CXCursor c)
{
    std::vector<std::string> parts;
    std::string self = cxs(clang_getCursorSpelling(c));
    if (!self.empty()) parts.push_back(self);
    for (CXCursor p = clang_getCursorSemanticParent(c);
         !clang_Cursor_isNull(p) && !clang_isTranslationUnit(clang_getCursorKind(p));
         p = clang_getCursorSemanticParent(p)) {
        std::string n = cxs(clang_getCursorSpelling(p));
        if (!n.empty()) parts.push_back(n);
    }
    std::string r;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) r += (r.empty() ? "" : "::") + *it;
    return r;
}

// Collects the direct bases of the classes that are defined in the user headers.
void collect_hierarchy(CXCursor root, Hierarchy& out)
{
    visit(root, [&](CXCursor c) {
        CXCursorKind k = clang_getCursorKind(c);
        if (k == CXCursor_Namespace) return CXChildVisit_Recurse;
        if ((k == CXCursor_ClassDecl || k == CXCursor_StructDecl) && clang_isCursorDefinition(c) &&
            !clang_Location_isInSystemHeader(clang_getCursorLocation(c))) {
            std::vector<std::string> bases;
            visit(c, [&](CXCursor bs) {
                if (clang_getCursorKind(bs) == CXCursor_CXXBaseSpecifier) {
                    CXCursor def = clang_getCursorDefinition(clang_getCursorReferenced(bs));
                    if (!clang_Cursor_isNull(def) && clang_getCursorKind(def) != CXCursor_NoDeclFound)
                        bases.push_back(qualified_name(def));
                    else
                        bases.push_back(tidy(cxs(clang_getTypeSpelling(clang_getCursorType(bs)))));
                }
                return CXChildVisit_Continue;
            });
            out[qualified_name(c)] = std::move(bases);
        }
        return CXChildVisit_Continue;
    });
}

// ----------------------------------------------------------------- emitter
struct Out {
    std::string s;
    int lines = 0;
    void w(const std::string& t)
    {
        s += t;
        lines += static_cast<int>(std::count(t.begin(), t.end(), '\n'));
    }
};

std::string esc(const std::string& f)
{
    std::string r;
    for (char c : f) {
        if (c == '\\' || c == '"') r += '\\';
        r += c;
    }
    return r;
}

std::string str_join(const std::vector<std::string>& v, const char* sep)
{
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) r += (i ? sep : "") + v[i];
    return r;
}

using sppc::RCtx;

// The name of a function for the stale call log: "Cls::name" or "Cls::name(types)".
std::string body_what(const std::string& cn, const Method& m)
{
    std::string what = cn + "::" + m.name;
    if (!m.overrides_virtual) return what;
    what += "(";
    for (size_t i = 0; i < m.params.size(); ++i) what += (i ? ", " : "") + m.params[i].type;
    return what + ")";
}

// ------------------------------------------------------------------ properties
// Changes each property into script members:
//  - An auto property gets the hidden field "__p_Name". The field keeps the initializer.
//    The state migration treats it as a normal field.
//  - Each accessor becomes a script fn: "Type get_Name()" and "void set_Name(Type value)".
//    The body of an auto accessor reads or writes the hidden field.
// The property stays in cd.props. The lowering pass uses it to rewrite reads and writes.
void desugar_props(ClassDef& cd, const sppc::LineMap& map)
{
    std::set<std::string> prop_names;
    for (const Property& p : cd.props) {
        if (!prop_names.insert(p.name).second) fail_at(map, p.pos, "property '" + p.name + "' is declared twice");
        for (const Field& f : cd.fields)
            if (f.name == p.name) fail_at(map, p.pos, "'" + p.name + "' is a field and a property");
        for (const Method& m : cd.methods) {
            if (m.name == p.name) fail_at(map, p.pos, "'" + p.name + "' is a method and a property");
            for (const char* word : {"get", "set"})
                if (m.name == std::string(word) + "_" + p.name)
                    fail_at(map, m.pos, "method '" + m.name + "' collides with the '" + word +
                                            "' accessor of property '" + p.name + "'");
        }
    }
    for (Property& p : cd.props) {
        const std::string hidden = "__p_" + p.name;
        auto this_hidden = [&] { return make_expr(MemberExpr{make_expr(ThisExpr{p.pos}), hidden, p.pos}); };
        if (p.is_auto()) {
            Field f;
            f.name = hidden;
            f.type = p.type;
            f.init = std::move(p.init);
            f.init_span = p.init_span;
            f.has_init = p.has_init;
            f.access = Access::Private;
            f.line = p.line;
            f.pos = p.pos;
            cd.fields.push_back(std::move(f));
        }
        auto accessor = [&](bool is_get) {
            Method m;
            m.access = is_get ? p.get_access : p.set_access;
            m.name = (is_get ? "get_" : "set_") + p.name;
            m.ret = is_get ? p.type : std::string();
            if (!is_get) m.params.push_back({"value", p.type});
            m.pos = is_get ? p.get_pos : p.set_pos;
            m.line = m.end_line = map.line(m.pos);
            m.end_pos = m.pos;
            std::optional<BlockStmt>& body = is_get ? p.get_body : p.set_body;
            if (body) {
                m.body = std::move(*body);  // the optional keeps a value: the accessor is custom
            } else if (is_get) {
                m.body.stmts.push_back(make_stmt(ReturnStmt{this_hidden(), m.pos}));
            } else {
                BinaryExpr b;
                b.op = "=";
                b.lhs = this_hidden();
                b.rhs = make_expr(NameExpr{{"value"}, "", {}, false, m.pos});
                b.pos = m.pos;
                m.body.stmts.push_back(make_stmt(ExprStmt{make_expr(std::move(b)), m.pos}));
            }
            cd.methods.push_back(std::move(m));
        };
        if (p.has_get) accessor(true);
        if (p.has_set) accessor(false);
    }
}

// ------------------------------------------------------------------ driver
struct Slot {
    std::string id;  // "Tick", "TakeDamage_f", "heal"
    const NativeVirtual* nv = nullptr;
    const Method* impl = nullptr;  // override (native slot) or function (script slot)
    int intro_gen = 0;             // script functions only
    bool native() const { return nv != nullptr; }
};

std::string mangle_type(const std::string& t)
{
    std::string n = norm(t);
    bool ptr = n.find('*') != std::string::npos;
    std::string core;
    for (size_t i = 0; i < n.size(); ++i) {
        if (n[i] == '&' || n[i] == '*') continue;
        core += n[i];
    }
    for (const char* q : {"const", "volatile"}) {
        size_t p = core.find(q);
        if (p == 0) core = core.substr(std::string(q).size());
    }
    static const std::map<std::string, std::string> builtin = {
        {"float", "f"},     {"double", "d"},       {"int", "i"},  {"bool", "b"},
        {"unsigned", "u"},  {"unsignedint", "u"},  {"char", "c"}, {"long", "l"},
        {"longlong", "ll"}, {"short", "s"},        {"void", "v"},
        {"int8_t", "i8"},   {"uint8_t", "u8"},     {"int16_t", "i16"},  {"uint16_t", "u16"},
        {"int32_t", "i"},   {"uint32_t", "u"},     {"int64_t", "i64"},  {"uint64_t", "u64"},
        {"intptr_t", "ip"}, {"uintptr_t", "up"},   {"char16_t", "w"},
    };
    std::string r;
    auto it = builtin.find(core);
    if (it != builtin.end()) {
        r = it->second;
    } else {
        size_t p = core.rfind(':');
        std::string last = core.substr(p == std::string::npos ? 0 : p + 1);
        r = last.empty() ? "x" : std::string(1, static_cast<char>(std::toupper(last[0])));
    }
    return ptr ? r + "P" : r;
}

std::string arg_expr(const std::string& type, const std::string& name)
{
    std::string n = norm(type);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "&&") == 0) return "std::move(" + name + ")";
    return name;
}

struct Options {
    std::string input, output;
    int gen = 0;
    std::vector<std::string> includes;
    std::string resource_dir;  // the Clang resource directory (builtin headers)
};

struct ManifestEntry {
    std::string cls, fn;
    int gen;
};

std::vector<ManifestEntry> read_manifest(const std::string& text)
{
    std::vector<ManifestEntry> v;
    std::istringstream f(text);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string name;
        int gen = 0;
        if (!(ss >> name >> gen)) continue;
        size_t p = name.find("::");
        if (p == std::string::npos) continue;
        v.push_back({name.substr(0, p), name.substr(p + 2), gen});
    }
    return v;
}

// A guard for the libclang handles. It releases them on every exit path.
struct ClangHandles {
    CXIndex idx = nullptr;
    CXTranslationUnit tu = nullptr;
    ~ClangHandles()
    {
        if (tu) clang_disposeTranslationUnit(tu);
        if (idx) clang_disposeIndex(idx);
    }
};

// The identity of a header file: the path, the time of the last change and the size.
// A file that changed lately also has a hash of its content. An edit can keep the time and
// the size, so the hash covers this case. A missing file has the time -1.
struct FileStamp {
    std::string path;
    long long mtime_ns = -1;
    long long size = -1;
    bool hashed = false;
    unsigned long long hash = 0;
};

// A file is recent when its time is less than this value before the check.
constexpr long long kRecentNs = 2000000000LL;

long long now_ns()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// FNV-1a hash of the content of a file.
unsigned long long hash_file(const std::string& path)
{
    unsigned long long h = 14695981039346656037ULL;
    std::ifstream f(path, std::ios::binary);
    char buf[8192];
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

// Reads the stamp. A file with a recent time also gets the hash of its content.
FileStamp stamp_of(const std::string& path)
{
    FileStamp f;
    f.path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        f.mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        f.size = static_cast<long long>(st.st_size);
        if (f.mtime_ns > now_ns() - kRecentNs) {
            f.hashed = true;
            f.hash = hash_file(path);
        }
    }
    return f;
}

// True when the file is the same as at the time of the record. The check reads the content
// only when the record has a hash. It drops the hash when the file is no longer recent.
bool stamp_fresh(FileStamp& rec)
{
    struct stat st;
    long long mtime = -1, size = -1;
    if (::stat(rec.path.c_str(), &st) == 0) {
        mtime = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        size = static_cast<long long>(st.st_size);
    }
    if (mtime != rec.mtime_ns || size != rec.size) return false;
    if (rec.hashed) {
        if (hash_file(rec.path) != rec.hash) return false;
        if (mtime <= now_ns() - kRecentNs) rec.hashed = false;
    }
    return true;
}

// Adds the stamps of the paths that a search of the include dirs tried before it found
// "rel". A new file at one of these paths would hide the file that the parse used.
void add_shadow_stamps(const std::string& rel, const std::vector<std::string>& dirs,
                       std::set<std::string>& seen, std::vector<FileStamp>& files)
{
    for (const auto& d : dirs) {
        const std::string cand = d + "/" + rel;
        struct stat st;
        if (::stat(cand.c_str(), &st) == 0) return;  // the first file that exists
        if (seen.insert(cand).second) files.push_back(stamp_of(cand));
    }
}

// The handle type of a stub: the template with "$T" replaced by the class name.
std::string handle_type(std::string tpl, const std::string& name)
{
    for (size_t p = 0; (p = tpl.find("$T", p)) != std::string::npos; p += name.size()) tpl.replace(p, 2, name);
    return tpl;
}

// The name of the probe section with the facts. libclang reports an error in it for a type
// that it does not know. The translator does not treat such an error as an error of the imports.
const char* const kFactsFile = "sppc_facts";

// The text that asks libclang for the facts of the types. The section follows the imports.
// Each fact is one constexpr variable on its own line. The variable "sppc_f<N>" is on the
// line N of the file "sppc_facts". The traits are in functions with "if constexpr", so a
// type that is not complete gives false and no error inside a standard header.
std::string facts_text(const std::vector<FactReq>& reqs, std::vector<std::pair<size_t, std::string>>& lines)
{
    std::string head =
        "#include <type_traits>\n"
        "template <class T> constexpr bool sppc_complete() { return requires { sizeof(T); }; }\n"
        "template <class T> constexpr bool sppc_copy() { if constexpr (sppc_complete<T>()) return std::is_copy_constructible_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_nothrow_move() { if constexpr (sppc_complete<T>()) return std::is_nothrow_move_constructible_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_polymorphic() { if constexpr (sppc_complete<T>()) return std::is_polymorphic_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_final() { if constexpr (sppc_complete<T>()) return std::is_final_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_plain() { if constexpr (sppc_complete<T>()) return std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>; else return false; }\n"
        "template <class T> constexpr bool sppc_arrow() { if constexpr (std::is_pointer_v<T>) return true; else if constexpr (sppc_complete<T>()) return requires(T& h) { h.operator->(); }; else return false; }\n";
    std::string body;
    lines.clear();
    size_t member_id = 0;
    for (size_t i = 0; i < reqs.size(); ++i) {
        const std::string& t = reqs[i].type;
        const char* kinds[] = {"is_class", "copy", "nothrow_move", "polymorphic", "final", "plain", "arrow"};
        for (const char* k : kinds) {
            lines.emplace_back(i, k);
            const std::string fn = std::string(k) == "is_class" ? "std::is_class_v<" + t + ">"
                                                                : std::string("sppc_") + k + "<" + t + ">()";
            body += "constexpr bool sppc_f" + std::to_string(lines.size()) + " = " + fn + ";\n";
        }
        for (const std::string& m : reqs[i].members) {
            const std::string fn = "sppc_member" + std::to_string(member_id++);
            head += "template <class T> constexpr bool " + fn + "() { return requires { &T::" + m + "; }; }\n";
            lines.emplace_back(i, "member:" + m);
            body += "constexpr bool sppc_f" + std::to_string(lines.size()) + " = " + fn + "<" + t + ">();\n";
        }
    }
    return head + "#line 1 \"" + kFactsFile + "\"\n" + body;
}

// Reads the value of "sppc_f<N>" from the translation unit. Gives -1 when libclang cannot
// evaluate it (the type is not known).
int eval_bool(CXCursor var)
{
    CXEvalResult r = clang_Cursor_Evaluate(var);
    int v = -1;
    if (r && clang_EvalResult_getKind(r) == CXEval_Int) v = clang_EvalResult_getAsInt(r) != 0 ? 1 : 0;
    if (r) clang_EvalResult_dispose(r);
    return v;
}

// Parses the imports with libclang. Reads the base classes in "names" that it finds.
// Gives the stamps of all files that the parse included.
void parse_bases(const std::string& probe, const std::vector<std::string>& args,
                 const std::set<std::string>& names, const std::set<std::string>& import_paths,
                 std::map<std::string, BaseInfo>& out, std::vector<FileStamp>& files,
                 Hierarchy& hierarchy, const std::vector<FactReq>& reqs, FactMap& facts)
{
    std::vector<std::pair<size_t, std::string>> fact_lines;
    const std::string full_probe = reqs.empty() ? probe : probe + facts_text(reqs, fact_lines);
    std::vector<const char*> cargs;
    for (const auto& a : args) cargs.push_back(a.c_str());
    ClangHandles ch;
    CXIndex idx = ch.idx = clang_createIndex(0, 0);
    CXUnsavedFile unsaved{"sppc_probe.cpp", full_probe.c_str(), static_cast<unsigned long>(full_probe.size())};
    CXTranslationUnit tu = nullptr;
    CXErrorCode ec = clang_parseTranslationUnit2(idx, "sppc_probe.cpp", cargs.data(),
                                                 static_cast<int>(cargs.size()), &unsaved, 1,
                                                 CXTranslationUnit_SkipFunctionBodies, &tu);
    ch.tu = tu;
    if (ec != CXError_Success || !tu) fail(0, "libclang cannot parse the imports");
    bool diag_error = false;
    for (unsigned i = 0, n = clang_getNumDiagnostics(tu); i < n; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);
        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
            // Use the presumed location, so "#line" gives the script name and line.
            CXString file;
            unsigned line = 0, col = 0;
            clang_getPresumedLocation(clang_getDiagnosticLocation(d), &file, &line, &col);
            if (cxs(file) == kFactsFile) {  // an unknown type of a fact: not an error of the imports
                clang_disposeDiagnostic(d);
                continue;
            }
            const char* sev = clang_getDiagnosticSeverity(d) == CXDiagnostic_Fatal ? "fatal error" : "error";
            g_diag.add_foreign(cxs(file), static_cast<int>(line), static_cast<int>(col), sev,
                               cxs(clang_getDiagnosticSpelling(d)));
            diag_error = true;
        }
        clang_disposeDiagnostic(d);
    }
    if (diag_error) fail(0, "the imports have errors");

    std::set<std::string> seen;
    auto on_include = [](CXFile f, CXSourceLocation*, unsigned depth, CXClientData d) {
        if (depth == 0) return;  // the probe file
        auto* v = static_cast<std::pair<std::set<std::string>*, std::vector<FileStamp>*>*>(d);
        std::string path = cxs(clang_getFileName(f));
        if (v->first->insert(path).second) v->second->push_back(stamp_of(path));
    };
    std::pair<std::set<std::string>*, std::vector<FileStamp>*> ctx{&seen, &files};
    files.clear();
    clang_getInclusions(tu, on_include, &ctx);

    // A new header earlier in the include path hides a header that the parse used.
    std::vector<std::string> dirs;
    for (const auto& a : args)
        if (a.size() > 2 && a.compare(0, 2, "-I") == 0) dirs.push_back(a.substr(2));
    for (const auto& ip : import_paths)
        if (!ip.empty() && ip.front() != '<') add_shadow_stamps(ip, dirs, seen, files);
        else if (ip.size() > 2) add_shadow_stamps(ip.substr(1, ip.size() - 2), dirs, seen, files);
    const std::vector<FileStamp> used = files;
    for (const FileStamp& f : used)
        for (const auto& d : dirs)
            if (f.mtime_ns >= 0 && f.path.size() > d.size() + 1 && f.path.compare(0, d.size() + 1, d + "/") == 0) {
                add_shadow_stamps(f.path.substr(d.size() + 1), dirs, seen, files);
                break;
            }

    CXCursor root = clang_getTranslationUnitCursor(tu);
    if (!reqs.empty()) {
        std::map<std::string, CXCursor> vars;
        visit(root, [&](CXCursor c) {
            if (clang_getCursorKind(c) == CXCursor_VarDecl) {
                const std::string n = cxs(clang_getCursorSpelling(c));
                if (n.compare(0, 6, "sppc_f") == 0) vars[n] = c;
            }
            return CXChildVisit_Continue;
        });
        for (size_t n = 0; n < fact_lines.size(); ++n) {
            auto it = vars.find("sppc_f" + std::to_string(n + 1));
            const int v = it == vars.end() ? -1 : eval_bool(it->second);
            TypeFacts& f = facts[reqs[fact_lines[n].first].type];
            const std::string& k = fact_lines[n].second;
            if (k == "is_class") {
                f.known = v >= 0;
                f.is_class = v == 1;
            } else if (k == "copy") f.copy = v == 1;
            else if (k == "nothrow_move") f.nothrow_move = v == 1;
            else if (k == "polymorphic") f.polymorphic = v == 1;
            else if (k == "final") f.final_ = v == 1;
            else if (k == "plain") f.plain = v == 1;
            else if (k == "arrow") f.arrow = v == 1;
            else f.members[k.substr(7)] = v == 1;
        }
    }
    hierarchy.clear();
    collect_hierarchy(root, hierarchy);
    for (const auto& name : names) {
        CXCursor base_cur = find_class(root, name);
        if (clang_Cursor_isNull(base_cur)) continue;
        BaseInfo bi;
        bi.name = name;
        collect_virtuals(base_cur, bi);
        collect_ctors(base_cur, bi);
        out[name] = std::move(bi);
    }
}

}  // namespace

namespace sppc {

// The cache of one key (flags + imports): the base classes and the files that the parse read.
struct TranslatorSession::Impl {
    struct Entry {
        std::vector<FileStamp> files;
        std::map<std::string, BaseInfo> bases;
        Hierarchy hierarchy;
        FactMap facts;
    };
    std::mutex mutex;
    std::map<std::string, Entry> entries;
    int parses = 0;
};

TranslatorSession::TranslatorSession() : impl_(new Impl) {}
TranslatorSession::~TranslatorSession() = default;

int TranslatorSession::parse_count() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->parses;
}

void TranslatorSession::clear()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->entries.clear();
}

}  // namespace sppc

namespace {

// Gives the base classes for one key. A cache hit needs all names in the entry and
// the same stamp for each file. Otherwise it parses again and keeps the new result.
std::map<std::string, BaseInfo> session_bases(
    sppc::TranslatorSession::Impl& S, const std::string& key, const std::set<std::string>& names,
    const std::vector<FactReq>& reqs,
    const std::function<void(std::map<std::string, BaseInfo>&, std::vector<FileStamp>&, Hierarchy&, FactMap&)>& parse,
    Hierarchy& hierarchy, FactMap& facts_out)
{
    std::lock_guard<std::mutex> lock(S.mutex);
    auto& e = S.entries[key];
    bool fresh = !e.files.empty();
    for (size_t i = 0; fresh && i < e.files.size(); ++i) fresh = stamp_fresh(e.files[i]);
    if (!fresh) e = {};
    bool complete = fresh;
    for (const auto& n : names) complete = complete && e.bases.count(n);
    for (const FactReq& r : reqs) {
        auto it = e.facts.find(r.type);
        complete = complete && it != e.facts.end();
        for (const std::string& m : r.members) complete = complete && it != e.facts.end() && it->second.members.count(m);
    }
    if (!complete) {
        std::map<std::string, BaseInfo> out = std::move(e.bases);
        FactMap facts = std::move(e.facts);
        std::vector<FileStamp> files;
        Hierarchy hier;
        ++S.parses;
        try {
            parse(out, files, hier, facts);
        } catch (...) {
            S.entries.erase(key);
            throw;
        }
        e.bases = std::move(out);
        e.files = std::move(files);
        e.hierarchy = std::move(hier);
        e.facts = std::move(facts);
    }
    hierarchy = e.hierarchy;
    for (const FactReq& r : reqs) facts_out[r.type] = e.facts[r.type];
    std::map<std::string, BaseInfo> r;
    for (const auto& n : names) {
        auto it = e.bases.find(n);
        if (it != e.bases.end()) r.emplace(n, it->second);
    }
    return r;
}

void translate_impl(const sppc::TranslateOptions& in, sppc::TranslatorSession::Impl& session,
                    sppc::TranslateResult& res)
{
    Options opt;
    opt.input = in.script_name;
    opt.output = in.output_name;
    opt.gen = in.generation;
    opt.includes = in.include_dirs;
    opt.resource_dir = in.resource_dir;
#ifdef SPP_RESOURCE_DIR
    if (opt.resource_dir.empty()) opt.resource_dir = SPP_RESOURCE_DIR;
#endif
    if (opt.gen < 1) fail(0, "the generation must be 1 or more");
    if (opt.output.empty()) opt.output = opt.input + ".cpp";
    g_script = opt.input;

    const sppc::LineMap map(in.script_text);
    Script sc = parse_structure(in.script_text, map);

    // ---- the type model: the preludes first, then the stubs of the script
    sppc::TypeModel types;
    // A stub with the place that holds it. The checks of the stubs (7.10.7) report there.
    struct StubSrc {
        const Stub* stub;
        const sppc::LineMap* map;
        std::string file;  // empty: the script
    };
    std::vector<StubSrc> all_stubs;
    struct PreludeData {
        std::string name;
        Script script;
        sppc::LineMap map;
    };
    std::deque<PreludeData> prelude_data;  // the addresses must stay
    for (const auto& [pname, ptext] : in.preludes) {
        const sppc::LineMap pmap(ptext);
        sppc::Diagnostics pd;
        Script ps = sppc::parse_script(ptext, pmap, pd);
        if (pd.items.empty() && !ps.classes.empty()) {
            const ClassDef& pc = ps.classes.front();
            pd.add(pc.pos, pmap, "a prelude can hold only stubs: '" + pname + "' has the class '" + pc.name + "'");
        }
        if (pd.items.empty()) types.add_stubs(ps.stubs, pd, pmap, sppc::TypeModel::Source::Prelude);
        if (!pd.items.empty()) {
            for (auto& d : pd.items) {
                d.file = pname;
                g_diag.items.push_back(std::move(d));
            }
            throw TranslateAbort{};
        }
        prelude_data.push_back({pname, std::move(ps), pmap});
        for (const Stub& st : prelude_data.back().script.stubs)
            all_stubs.push_back({&st, &prelude_data.back().map, pname});
    }
    types.add_stubs(sc.stubs, g_diag, map, sppc::TypeModel::Source::Script);
    if (!g_diag.items.empty()) throw TranslateAbort{};
    for (const Stub& st : sc.stubs) all_stubs.push_back({&st, &map, std::string()});
    for (const ClassDef& cd : sc.classes)
        if (cd.is_value) fail_at(map, cd.pos, "[value] on a script class is not supported yet");

    // ---- the questions for libclang about the types (design 7.10.7 and the stub "class *;")
    std::map<std::string, std::set<std::string>> asked;  // type text, member names
    auto is_exact = [](const std::string& n) {
        return n != "*" && n.find('*') == std::string::npos;
    };
    for (const StubSrc& ss : all_stubs) {
        const Stub& st = *ss.stub;
        if (!is_exact(st.name)) continue;
        asked[st.name];
        if (!st.intrusive_add.empty() || !st.intrusive_release.empty()) {
            asked[st.name].insert(st.intrusive_add);
            asked[st.name].insert(st.intrusive_release);
        } else if (!st.handle.empty()) {
            asked[handle_type(st.handle, st.name)];
        }
    }
    for (const std::string& t : sppc::used_type_names(sc))
        if (types.wants_facts(t)) asked[t];
    std::vector<FactReq> reqs;
    for (auto& [t, ms] : asked) reqs.push_back({t, std::vector<std::string>(ms.begin(), ms.end())});

    // ---- libclang: parse the imports and read the native base classes.
    std::string probe;
    for (const auto& im : sc.imports)
        probe += "#line " + std::to_string(im.line) + " \"" + esc(opt.input) + "\"\n#include " +
                 im.target() + "\n";
    std::vector<std::string> args = {"-x", "c++", "-std=c++20"};
    size_t slash = opt.input.rfind('/');
    args.push_back("-I" + (slash == std::string::npos ? std::string(".") : opt.input.substr(0, slash)));
    for (const auto& d : opt.includes) args.push_back("-I" + d);
    if (!opt.resource_dir.empty()) args.push_back("-resource-dir=" + opt.resource_dir);
    std::set<std::string> import_paths, base_names;
    for (const auto& im : sc.imports) import_paths.insert(im.path);
    for (const auto& cd : sc.classes) base_names.insert(cd.base);
    // The key: the flags, the current directory and the imports (the order does not matter).
    std::string key = std::filesystem::current_path().string() + "\n";
    for (const auto& a : args) key += a + "\n";
    for (const auto& p : import_paths) key += "#" + p + "\n";
    Hierarchy hier;  // must outlive every use of "types"
    FactMap facts;
    const std::map<std::string, BaseInfo> bases =
        session_bases(session, key, base_names, reqs,
                      [&](std::map<std::string, BaseInfo>& out, std::vector<FileStamp>& files, Hierarchy& h,
                          FactMap& fm) {
                          parse_bases(probe, args, base_names, import_paths, out, files, h, reqs, fm);
                      },
                      hier, facts);
    types.set_classes([&facts](const std::string& name) {
        auto it = facts.find(name);
        sppc::TypeModel::ClassFacts c;
        if (it != facts.end()) {
            c.known = it->second.known && it->second.is_class;
            c.plain = it->second.plain;
        }
        return c;
    });

    // ---- the checks of the stubs (design 7.10.7). A type that libclang does not know is skipped.
    for (const StubSrc& ss : all_stubs) {
        const Stub& st = *ss.stub;
        if (!is_exact(st.name)) continue;
        auto err = [&](const std::string& msg) {
            g_diag.add(st.pos, *ss.map, msg);
            g_diag.items.back().file = ss.file;
        };
        auto tf = facts.find(st.name);
        if (tf == facts.end() || !tf->second.known) continue;
        const TypeFacts& f = tf->second;
        if (st.is_value) {
            if (!f.copy) err("'value' needs a copy constructor: '" + st.name + "' has none");
            if (!f.nothrow_move)
                err("'value' needs a nothrow move constructor: '" + st.name +
                    "' has none. The reload moves the state block with it");
            if (f.polymorphic && !f.final_)
                err("'value' on the polymorphic type '" + st.name +
                    "' is not allowed, because a copy can slice. Mark the type 'final' or use a handle");
        }
        if (!st.intrusive_add.empty() || !st.intrusive_release.empty()) {
            for (const std::string& m : {st.intrusive_add, st.intrusive_release}) {
                auto mit = f.members.find(m);
                if (mit != f.members.end() && !mit->second)
                    err("'intrusive' method '" + m + "' is not a usable member of '" + st.name +
                        "'. It must exist and must not be overloaded");
            }
        } else if (!st.handle.empty()) {
            const std::string ht = handle_type(st.handle, st.name);
            auto hit = facts.find(ht);
            if (hit != facts.end() && hit->second.known && !hit->second.arrow)
                err("'handle' needs 'operator->': the handle type '" + ht + "' has none");
        }
    }
    if (!g_diag.items.empty()) throw TranslateAbort{};


    types.set_bases([&hier](const std::string& name) {
        auto it = hier.find(name);
        return it == hier.end() ? std::vector<std::string>{} : it->second;
    });

    std::vector<ManifestEntry> old_manifest;
    {
        // Keep the entries of the earlier generations. Keep also the entries of
        // the classes that this script does not define (an other script owns them).
        std::set<std::string> mine;
        for (const auto& cd : sc.classes) mine.insert(cd.name);
        for (auto& e : read_manifest(in.slots_in))
            if (e.gen < opt.gen || !mine.count(e.cls)) old_manifest.push_back(e);
    }
    std::vector<ManifestEntry> new_manifest = old_manifest;

    const bool multi = sc.classes.size() > 1;
    Out o;
    o.w("// Generated by sppc from " + opt.input + " (generation " + std::to_string(opt.gen) +
        "). Do not edit.\n");
    o.w("#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
        "#include <new>\n#include <string>\n#include <type_traits>\n#include <typeinfo>\n"
        "#include <utility>\n\n#include \"spp/runtime.h\"\n\n");
    auto reset = [&] { o.w("#line " + std::to_string(o.lines + 2) + " \"" + esc(opt.output) + "\"\n"); };
    for (const auto& im : sc.imports) {
        o.w("#line " + std::to_string(im.line) + " \"" + esc(opt.input) + "\"\n");
        o.w("#include " + im.target() + "\n");
    }
    reset();
    o.w("\nconstexpr uint32_t GEN = " + std::to_string(opt.gen) + ";\n");

    std::vector<std::pair<std::string, bool>> registers;  // class name, for the register function
    std::vector<std::string> thunk_regs;
    // The script classes that a "new" expression names (design 7.3). They get a "new" function.
    const std::set<std::string> new_set = sppc::new_targets(sc);
    std::set<std::string> new_ready;  // the classes with a "new" function that are emitted so far

    for (ClassDef& cd : sc.classes) {
        const std::string cn = cd.name;
        const bool has_ctor = !cd.ctors.empty(), has_dtor = !cd.dtors.empty();
        const bool gen_new = has_ctor || new_set.count(cn) > 0;
        const std::string P = cn + "__";
        auto slot_id = [&](const std::string& n) { return (multi ? cn + "_" : std::string()) + "SLOT_" + n; };

        // ---- base class information from libclang
        auto base_it = bases.find(cd.base);
        if (base_it == bases.end())
            fail_at(map, cd.pos, "base class '" + cd.base + "' is not found in the imported headers");
        BaseInfo bi = base_it->second;
        if (!bi.any_ctor) bi.default_ctor = true;  // the compiler declares one
        if (!bi.virtual_dtor)
            fail_at(map, cd.pos, "base class '" + cd.base + "' has no virtual destructor");
        std::string ctor_args;
        if (bi.ctor_kind == 1) ctor_args = ", std::string(\"" + cn + "\")";
        else if (bi.ctor_kind == 2) ctor_args = ", \"" + cn + "\"";
        else if (!bi.default_ctor && !(has_ctor && cd.ctors[0].has_super))
            fail_at(map, cd.pos, "base class '" + cd.base + "' has no default constructor and no constructor that takes a name");

        // ---- check the script members
        desugar_props(cd, map);
        {
            const size_t before = g_diag.items.size();
            sppc::check_ctors(cd, map, g_diag);
            if (g_diag.items.size() != before) throw TranslateAbort{};
        }
        for (const sppc::ast::Ctor& c : cd.ctors)
            for (const Param& p : c.params)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail_at(map, c.pos, "parameter name '" + p.name + "' is reserved");
        std::set<std::string> field_names;
        for (const Field& f : cd.fields)
            if (!field_names.insert(f.name).second) fail_at(map, f.pos, "field '" + f.name + "' is declared twice");
        // The hooks of design 4.7: "void onReload(OldState old)" and "convert_<field>(Old old)".
        // They are not script functions: they get no slot and no thunk.
        enum class Hook { None, OnReload, Convert };
        auto hook_of = [&](const Method& m) {
            if (m.overrides_virtual) return Hook::None;
            if (m.name == "onReload") return Hook::OnReload;
            if (m.name.rfind("convert_", 0) == 0 && field_names.count(m.name.substr(8)))
                return Hook::Convert;
            return Hook::None;
        };
        std::set<std::string> fn_names;
        std::map<std::string, const Method*> overrides;  // by native key
        for (Method& m : cd.methods) {
            for (const Param& p : m.params)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail_at(map, m.pos, "parameter name '" + p.name + "' is reserved");
            // A method overrides a base virtual method when the name and the parameter types match.
            // The method must have the modifier "override" (C# rules).
            std::string key = m.name + "(";
            for (const Param& p : m.params)
                key += key_norm(p.type) + (p.mode == PassMode::Value ? "" : "&") + ",";
            const NativeVirtual* hit = nullptr;
            for (const auto& nv : bi.virtuals) {
                std::string k = nv.key();
                if (nv.is_const) k.resize(k.size() - 5);
                if (k == key + ")") hit = &nv;
            }
            if (!hit) {
                // A virtual method with the same name and other parameters: the function hides it.
                const NativeVirtual* same_name = nullptr;
                std::string cands;
                for (const auto& nv : bi.virtuals)
                    if (nv.name == m.name) {
                        if (!same_name) same_name = &nv;
                        cands += (cands.empty() ? "" : "; ") + nv.text();
                    }
                if (same_name) {
                    std::string msg = "function '" + m.name + "' hides the virtual method of " + same_name->owner;
                    if (!cands.empty()) msg += ". Candidates: " + cands;
                    fail_at(map, m.pos, msg);
                }
                if (m.is_override) fail_at(map, m.pos, "'override' has no virtual method to override");
                if (!fn_names.insert(m.name).second)
                    fail_at(map, m.pos, "function '" + m.name + "' is declared twice (overloads are not supported)");
                if (field_names.count(m.name)) fail_at(map, m.pos, "'" + m.name + "' is a field and a function");
                continue;
            }
            if (m.is_new) fail_at(map, m.pos, "'new' cannot hide a virtual method; write 'override'");
            if (!m.is_override)
                fail_at(map, m.pos, "method '" + m.name + "' overrides a virtual method of " + hit->owner +
                                  "; write 'override'");
            m.overrides_virtual = true;
            if (hit->final_)
                fail_at(map, m.pos, "'" + hit->text() + "' of " + hit->owner + " is final and cannot be overridden");
            if (!m.ret.empty() && key_norm(m.ret) != key_norm(hit->ret))
                fail_at(map, m.pos, "return type '" + m.ret + "' differs from '" + hit->ret + "'");
            if (!overrides.emplace(hit->key(), &m).second)
                fail_at(map, m.pos, "'" + hit->text() + "' is overridden twice");
        }
        for (const auto& nv : bi.virtuals)
            if (nv.pure && !overrides.count(nv.key()))
                fail_at(map, cd.pos, "class " + cn + " does not override the pure virtual method '" +
                                  nv.text() + "' of " + nv.owner);

        // ---- the hooks (design 4.7)
        for (const Method& m : cd.methods) {
            const Hook h = hook_of(m);
            if (h == Hook::None) continue;
            if (m.is_override || m.is_virtual)
                fail_at(map, m.pos, "the hook '" + m.name + "' cannot be 'override' or 'virtual'");
            if (h == Hook::OnReload) {
                if (!m.ret.empty() && m.ret != "void")
                    fail_at(map, m.pos, "'onReload' must return void");
                if (m.params.size() != 1 || m.params[0].type != "OldState" ||
                    m.params[0].mode != PassMode::Value)
                    fail_at(map, m.pos, "'onReload' takes one parameter: 'OldState old'");
            } else {
                if (m.params.size() != 1 || m.params[0].mode != PassMode::Value)
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It takes one parameter: the old value");
                if (m.ret.empty() || m.ret == "void")
                    fail_at(map, m.pos, "'" + m.name + "' is a convert hook. It must return the new field type");
            }
        }

        // ---- the borrowed-storage rule and the rows of design 5.3 that the script text shows
        {
            const size_t before = g_diag.items.size();
            sppc::check_class(cd, types, map, g_diag, in.strict_borrowed);
            if (g_diag.items.size() != before) throw TranslateAbort{};
        }

        // ---- the C++ storage types. An override keeps the native types.
        for (Field& f : cd.fields) f.type = types.storage(f.type);
        for (Method& m : cd.methods) {
            if (m.overrides_virtual) {
                // An override keeps the native type. A "ref" or "out" parameter is a reference.
                for (Param& p : m.params)
                    if (p.mode != PassMode::Value) p.type += "&";
                continue;
            }
            const Hook h = hook_of(m);
            if (h == Hook::OnReload) {
                m.params[0].type = "const spp::OldState&";
                m.ret = "void";
                continue;
            }
            if (h == Hook::Convert) {  // the old value is a copy of the old field type
                m.params[0].type = types.storage(m.params[0].type);
                m.ret = types.storage(m.ret);
                const std::string fname = m.name.substr(8);
                for (const Field& f : cd.fields)
                    if (f.name == fname && f.type != m.ret)
                        fail_at(map, m.pos, "'" + m.name + "' must return the type of the field: '" + f.type + "'");
                continue;
            }
            for (Param& p : m.params)
                p.type = p.mode == PassMode::Value ? types.param(p.type) : types.ref_param(p.type);
            if (!m.ret.empty()) m.ret = types.storage(m.ret);
        }

        for (sppc::ast::Ctor& c : cd.ctors)
            for (Param& p : c.params) p.type = types.param(p.type);

        // ---- slots: native virtuals first, then the script functions
        std::vector<Slot> slots;
        std::map<std::string, int> name_count;
        // A final method gets no slot: the shell cannot override it.
        for (const auto& nv : bi.virtuals)
            if (!nv.final_) ++name_count[nv.name];
        std::set<std::string> used;
        for (const auto& nv : bi.virtuals) {
            if (nv.final_) continue;
            Slot s;
            s.nv = &nv;
            s.id = nv.name;
            if (name_count[nv.name] > 1) {
                std::string suf;
                for (const auto& p : nv.ptypes) suf += mangle_type(p);
                s.id += "_" + (suf.empty() ? std::string("v") : suf);
            }
            for (int n = 2; used.count(s.id); ++n) s.id += std::to_string(n);
            used.insert(s.id);
            auto it = overrides.find(nv.key());
            if (it != overrides.end()) s.impl = it->second;
            slots.push_back(s);
        }
        const size_t native_count = slots.size();
        std::map<std::string, const Method*> fn_by_name;
        for (const Method& m : cd.methods)
            if (!m.overrides_virtual && hook_of(m) == Hook::None) fn_by_name[m.name] = &m;
        for (const auto& e : old_manifest) {
            if (e.cls != cn) continue;
            auto it = fn_by_name.find(e.fn);
            if (it == fn_by_name.end())
                fail_at(map, cd.pos, "function '" + e.fn + "' of an earlier generation is removed. The slot count must not shrink");
            Slot s;
            s.id = e.fn;
            s.impl = it->second;
            s.intro_gen = e.gen;
            slots.push_back(s);
        }
        for (const Method& m : cd.methods) {
            if (m.overrides_virtual || hook_of(m) != Hook::None) continue;
            bool known = false;
            for (const auto& s : slots) known |= !s.native() && s.id == m.name;
            if (known) continue;
            Slot s;
            s.id = m.name;
            s.impl = &m;
            s.intro_gen = opt.gen;
            slots.push_back(s);
            new_manifest.push_back({cn, m.name, opt.gen});
        }

        // ---- lowering context
        RCtx base_cx = sppc::make_class_ctx(cd);
        base_cx.map = &map;
        base_cx.types = &types;
        for (const auto& sc_cls : sc.classes) base_cx.script_classes.insert(sc_cls.name);
        if (gen_new) new_ready.insert(cn);
        base_cx.new_classes = new_ready;

        const std::string S = cn + "State_g" + std::to_string(opt.gen);
        const std::string shell = cn + "_Shell";
        const std::string B = cd.base;

        o.w("\n// ---------------------------------------------------------------- class " + cn + "\n");
        o.w("// Slot numbers. Native virtuals come first, in the order of " + B + ".\n");
        o.w("enum : unsigned {\n");
        for (size_t i = 0; i < slots.size(); ++i)
            o.w("    " + slot_id(slots[i].id) + " = " + std::to_string(i) + ",\n");
        o.w("    " + slot_id("COUNT") + " = " + std::to_string(slots.size()) + ",\n};\n\n");
        o.w("extern spp::ClassRecord " + cn + "_record;\n\n");

        // ---- shell
        o.w("// The shell overrides every virtual of " + B + ". Each stub reads one slot.\n");
        o.w("struct " + shell + " final : " + B + ", spp::ShellBase {\n");
        o.w("    template <class... A>\n    explicit " + shell + "(spp::Construct, A&&... a) : " + B +
            "(std::forward<A>(a)...)\n    {\n        spp::init_state(this, &" + cn +
            "_record);\n    }\n    ~" + shell + "() override { spp::destroy_state(this); }\n");
        for (size_t i = 0; i < native_count; ++i) {
            const NativeVirtual& nv = *slots[i].nv;
            std::vector<std::string> ps, sig = {B + "*"}, as = {nv.is_const ? "const_cast<" + shell + "*>(this)" : "this"};
            for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                ps.push_back(nv.ptypes[k] + " a" + std::to_string(k));
                sig.push_back(nv.ptypes[k]);
                as.push_back(arg_expr(nv.ptypes[k], "a" + std::to_string(k)));
            }
            o.w("\n    " + nv.ret + " " + nv.name + "(" + str_join(ps, ", ") + ")" +
                (nv.is_const ? " const" : "") + (nv.no_except ? " noexcept" : "") + " override\n    {\n");
            o.w("        return spp::call<" + nv.ret + "(" + str_join(sig, ", ") + ")>(cls, " +
                slot_id(slots[i].id) + ", " + str_join(as, ", ") + ");\n    }\n");
        }
        o.w("};\n\n");

        // ---- state struct and field table
        o.w("// ---------------------------------------------------------------- state\n");
        o.w("struct " + S + " {\n");
        for (const Field& f : cd.fields) o.w("    " + f.type + " " + f.name + ";\n");
        if (cd.fields.empty()) o.w("    char unused_;\n");
        o.w("};\n\n");
        for (Field& f : cd.fields) {
            RCtx fc = base_cx;
            fc.allow_this = false;
            std::string init;
            if (f.has_init && f.init) {
                sppc::lower_expr(f.init, fc, g_diag);
                if (!g_diag.items.empty()) throw TranslateAbort{};
                init = sppc::ast::print(*f.init);
            }
            o.w("#line " + std::to_string(f.line) + " \"" + esc(opt.input) + "\"\n");
            o.w("static void " + P + "init_" + f.name + "(void*, void* f) { using T = " + f.type +
                "; new (f) T(" + init + "); }\n");
            reset();
        }
        // A convert hook of a field (design 4.7). The wrapper builds the new field from the old one.
        std::map<std::string, const Method*> converts;  // by field name
        for (const Method& m : cd.methods)
            if (hook_of(m) == Hook::Convert) converts[m.name.substr(8)] = &m;
        for (const auto& [fname, m] : converts) {
            o.w("static " + m->ret + " " + P + m->name + "(" + m->params[0].type + " " + m->params[0].name + ");\n");
            o.w("static void " + P + m->name + "_hook(void* dst, const void* src)\n{\n    new (dst) " + m->ret + "(" +
                P + m->name + "(*static_cast<const " + m->params[0].type + "*>(src)));\n}\n");
        }
        o.w("\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n");
        if (cd.fields.empty()) {
            o.w("static const spp::FieldInfo* const " + cn + "_fields = nullptr;\n");
        } else {
            o.w("static const spp::FieldInfo " + cn + "_fields[] = {\n");
            for (const Field& f : cd.fields) {
                const std::string T = f.type;
                o.w("    {\"" + f.name + "\", " + (f.alias.empty() ? "nullptr" : "\"" + f.alias + "\"") +
                    ", typeid(" + T + ").name(), offsetof(" + S + ", " + f.name + "), sizeof(" + T +
                    "),\n     alignof(" + T + "), spp::field_flags<" + T + ">(), " + P + "init_" +
                    f.name + ", spp::move_field<" + T + ">, spp::field_destroyer<" + T + ">()");
                auto cv = converts.find(f.name);
                if (cv != converts.end())
                    o.w(", typeid(" + cv->second->params[0].type + ").name(), " + P + cv->second->name + "_hook");
                o.w("},\n");
            }
            o.w("};\n");
        }
        o.w("#pragma clang diagnostic pop\n\n");
        o.w("static const spp::StateLayout " + cn + "_layout = {\n    \"" + S + "\", sizeof(" + S +
            "), alignof(" + S + "), " + std::to_string(cd.fields.size()) + ", " + cn + "_fields,\n};\n\n");

        // ---- generation functions
        o.w("// ------------------------------------------------- generation functions\n");
        o.w("static inline " + S + "* state_of(" + shell + "* self)\n{\n    return static_cast<" + S +
            "*>(self->state);\n}\n\n");

        auto param_decl = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + p.type + " " + p.name;
            return r;
        };
        auto fn_ret = [](const Method& m) { return m.ret.empty() ? std::string("void") : m.ret; };
        auto type_list = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + p.type;
            return r;
        };
        auto arg_list = [](const Method& m) {
            std::string r;
            for (const Param& p : m.params) r += ", " + arg_expr(p.type, p.name);
            return r;
        };

        // forward declarations of the script functions
        for (const Method& m : cd.methods)
            if (!m.overrides_virtual && hook_of(m) != Hook::Convert)
                o.w("static " + fn_ret(m) + " " + P + m.name + "(" + shell + "* self" + param_decl(m) + ");\n");
        o.w("\n");

        // The constructor (design 7.3). The constructor body is not a slot. A "new" function
        // builds the shell in the module that made the class, then runs the body here.
        Method ctor_m;
        if (has_ctor) {
            ctor_m.name = cn;
            ctor_m.params = cd.ctors[0].params;
            ctor_m.pos = cd.ctors[0].pos;
            o.w("static void " + P + "ctor(" + shell + "* self" + param_decl(ctor_m) + ");\n");
        }
        if (has_dtor) o.w("static void " + P + "dtor(" + shell + "* self);\n");
        if (gen_new) {
            std::string pd = param_decl(ctor_m), names, declvals, defaults;
            if (!pd.empty()) pd = pd.substr(2);
            for (size_t i = 0; i < ctor_m.params.size(); ++i) {
                const Param& p = ctor_m.params[i];
                names += (i ? ", " : "") + p.name;
                declvals += (i ? ", " : "") + ("std::declval<" + p.type + ">()");
                defaults += (i ? ", " : "") + ("std::decay_t<" + p.type + ">()");
            }
            std::string ctor_fwd = arg_list(ctor_m);
            std::string sup_text;
            int sup_line = cd.line;
            if (has_ctor) sup_line = cd.ctors[0].line;
            if (has_ctor && cd.ctors[0].has_super) {
                RCtx fc = base_cx;
                fc.allow_this = false;
                fc.where = cn + "::" + cn;
                sppc::add_params(fc, ctor_m.params);
                std::vector<std::string> parts;
                for (ExprP& a : cd.ctors[0].super_args) {
                    sppc::lower_expr(a, fc, g_diag);
                    if (!g_diag.items.empty()) throw TranslateAbort{};
                    parts.push_back(sppc::ast::print(*a));
                }
                sup_text = str_join(parts, ", ");
            } else if (!ctor_args.empty()) {
                sup_text = ctor_args.substr(2);
            }
            o.w("#line " + std::to_string(sup_line) + " \"" + esc(opt.input) + "\"\n");
            o.w("[[maybe_unused]] static auto " + P + "super_args(" + pd + ")\n{\n    return std::make_tuple(" + sup_text +
                ");\n}\n");
            reset();
            o.w("using " + cn + "_SuperArgs = decltype(" + P + "super_args(" + declvals + "));\n");
            o.w("using " + cn + " = " + shell + ";\n\n");
            o.w("// Makes the shell with the base arguments. Only the first generation of the class runs it.\n");
            o.w("static spp::ShellBase* " + P + "construct(const void* args)\n{\n    const auto& t = *static_cast<const " +
                cn + "_SuperArgs*>(args);\n    return std::apply([](const auto&... x) -> spp::ShellBase* { return new " +
                shell + "(spp::Construct{}, x...); }, t);\n}\n\n");
            o.w("[[maybe_unused]] static " + shell + "* " + P + "new(" + pd + ")\n{\n");
            o.w("    auto sa = " + P + "super_args(" + names + ");\n");
            o.w("    auto* self = static_cast<" + shell + "*>(spp::construct_shell(&" + cn + "_record, typeid(" + cn +
                "_SuperArgs).name(), &sa));\n");
            if (has_ctor) {
                o.w("    try {\n        " + P + "ctor(self" + ctor_fwd + ");\n    } catch (...) {\n        delete static_cast<" + B +
                    "*>(self);\n        throw;\n    }\n");
            }
            o.w("    return self;\n}\n\n");
            if (has_ctor) {
                o.w("static void " + P + "ctor_default(spp::ShellBase* s)\n{\n    " + P + "ctor(static_cast<" + shell +
                    "*>(s)" + (defaults.empty() ? "" : ", " + defaults) + ");\n}\n\n");
            }
        }
        if (has_dtor)
            o.w("static void " + P + "dtor_slot(spp::ShellBase* s)\n{\n    " + P + "dtor(static_cast<" + shell +
                "*>(s));\n}\n\n");

        auto emit_body = [&](Method& m, bool is_fn, const std::string& sym, const std::string& ret,
                             const std::string& what) {
            RCtx cx = base_cx;
            cx.where = what;
            sppc::add_params(cx, m.params);
            std::string head = "static " + ret + " " + sym + "(";
            if (is_fn) head += shell + "* self" + param_decl(m);
            else head += B + "* a" + param_decl(m);
            o.w(head + ")\n{\n");
            if (!is_fn) o.w("    [[maybe_unused]] auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    if (SPP_UNLIKELY(spp::active_generation != GEN)) {\n");
            o.w("        spp::stale_call(\"" + what + "\");\n        return" +
                (ret == "void" ? std::string() : " " + ret + "()") + ";\n    }\n");
            o.w("    [[maybe_unused]] auto* st = state_of(self);\n");
            sppc::lower_block(m.body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            sppc::ast::Emitter em(map, opt.input);
            sppc::ast::emit_stmts(m.body.stmts, em, 1);
            em.finish();
            o.w(em.text());
            reset();
            o.w("}\n\n");
        };

        // A convert hook has no self and no state: the new block is not complete yet.
        auto emit_convert = [&](Method& m) {
            RCtx cx = base_cx;
            cx.where = body_what(cn, m);
            cx.allow_this = false;
            cx.field_type.clear();
            cx.fns.clear();
            sppc::add_params(cx, m.params);
            o.w("static " + m.ret + " " + P + m.name + "(" + m.params[0].type + " " + m.params[0].name + ")\n{\n");
            sppc::lower_block(m.body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            sppc::ast::Emitter em(map, opt.input);
            sppc::ast::emit_stmts(m.body.stmts, em, 1);
            em.finish();
            o.w(em.text());
            reset();
            o.w("}\n\n");
        };

        if (has_ctor) {
            ctor_m.body = std::move(cd.ctors[0].body);
            emit_body(ctor_m, true, P + "ctor", "void", cn + "::" + cn);
        }
        if (has_dtor) emit_body(cd.dtors[0], true, P + "dtor", "void", cn + "::~" + cn);
        for (Method& m : cd.methods) {
            if (hook_of(m) == Hook::Convert) {
                emit_convert(m);
            } else if (m.overrides_virtual) {
                const Slot* s = nullptr;
                for (const Slot& x : slots)
                    if (x.impl == &m) s = &x;
                emit_body(m, false, P + s->id, s->nv->ret, body_what(cn, m));
            } else {
                emit_body(m, true, P + m.name, fn_ret(m), body_what(cn, m));
            }
        }

        // forwarders for the native virtuals that the script does not override
        for (size_t i = 0; i < native_count; ++i) {
            const Slot& s = slots[i];
            if (s.impl) continue;
            const NativeVirtual& nv = *s.nv;
            std::string ps, as;
            for (size_t k = 0; k < nv.ptypes.size(); ++k) {
                ps += ", " + nv.ptypes[k] + " a" + std::to_string(k);
                as += (k ? ", " : "") + arg_expr(nv.ptypes[k], "a" + std::to_string(k));
            }
            o.w("// Not overridden: call the implementation of " + B + ".\n");
            o.w("static " + nv.ret + " " + P + "fwd_" + s.id + "(" + B + "* a" + ps + ")\n{\n");
            o.w("    return static_cast<" + shell + "*>(a)->" + B + "::" + nv.name + "(" + as + ");\n}\n\n");
        }

        // slot adapters and thunks for the script functions
        for (size_t i = native_count; i < slots.size(); ++i) {
            const Method& m = *slots[i].impl;
            o.w("static " + fn_ret(m) + " " + P + m.name + "_slot(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    return " + P + m.name + "(static_cast<" + shell + "*>(a)" + arg_list(m) + ");\n}\n\n");
        }
        for (size_t i = native_count; i < slots.size(); ++i) {
            if (slots[i].intro_gen != opt.gen) continue;
            const Method& m = *slots[i].impl;
            o.w("// The thunk has the same address in each generation (section 4.11). The\n"
                "// generation that adds the function emits it. It reads the slot at call time.\n");
            o.w("static " + fn_ret(m) + " " + P + m.name + "__thunk(" + B + "* a" + param_decl(m) + ")\n{\n");
            o.w("    auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    return spp::call<" + fn_ret(m) + "(" + B + "*" + type_list(m) + ")>(self->cls, " +
                slot_id(m.name) + ", a" + arg_list(m) + ");\n}\n\n");
            thunk_regs.push_back("    reg.add_thunk(\"" + cn + "::" + m.name +
                                 "\", reinterpret_cast<void*>(&" + P + m.name + "__thunk));\n");
        }

        // ---- record and entry
        o.w("// ------------------------------------------------------ record and entry\n");
        o.w("static void* " + cn + "_slots[" + slot_id("COUNT") + "] = {\n");
        for (size_t i = 0; i < slots.size(); ++i) {
            std::string fn;
            if (!slots[i].native()) fn = P + slots[i].id + "_slot";
            else fn = P + (slots[i].impl ? "" : "fwd_") + slots[i].id;
            o.w("    reinterpret_cast<void*>(&" + fn + "),\n");
        }
        o.w("};\n\n");
        std::string on_reload_init;
        for (const Method& m : cd.methods)
            if (hook_of(m) == Hook::OnReload) {
                o.w("static void " + P + "onReload_hook(spp::ShellBase* s, const spp::OldState& old)\n{\n    " + P +
                    "onReload(static_cast<" + shell + "*>(s), old);\n}\n\n");
                on_reload_init = " " + P + "onReload_hook,\n";
            }
        // After on_reload: construct, construct_key, ctor_default, canonical and dtor.
        std::string ctor_init;
        if (gen_new || has_dtor)
            ctor_init = std::string(on_reload_init.empty() ? "    nullptr,\n" : "") + "    " +
                        (gen_new ? "&" + P + "construct, typeid(" + cn + "_SuperArgs).name()" : "nullptr, nullptr") +
                        ", " + (has_ctor ? "&" + P + "ctor_default" : std::string("nullptr")) + ", nullptr, " +
                        (has_dtor ? "&" + P + "dtor_slot" : std::string("nullptr")) + ",\n";
        o.w("spp::ClassRecord " + cn + "_record = {\n    \"" + cn + "\", GEN, " + cn + "_slots, " +
            slot_id("COUNT") + ", &" + cn + "_layout, nullptr,\n    \"" + B + "\", spp::shell_layout_hash(\"" +
            B + "\", sizeof(" + shell + "), alignof(" + shell + "), " + std::to_string(native_count) +
            "),\n" + (on_reload_init.empty() ? "" : "   " + on_reload_init) + ctor_init + "};\n\n");
        if (has_ctor) {
            // The factory of the engine has no arguments. It builds the base and runs the
            // current constructor with default values.
            std::string defaults;
            for (size_t i = 0; i < ctor_m.params.size(); ++i)
                defaults += (i ? ", " : "") + ("std::decay_t<" + ctor_m.params[i].type + ">()");
            o.w("static spp::Instance " + cn + "_create()\n{\n    auto sa = " + P + "super_args(" + defaults + ");\n" +
                "    auto* p = static_cast<" + shell + "*>(" + P + "construct(&sa));\n" +
                "    try {\n        if (" + cn + "_record.ctor_default) " + cn + "_record.ctor_default(p);\n" +
                "    } catch (...) {\n        delete static_cast<" + B + "*>(p);\n        throw;\n    }\n" +
                "    return {static_cast<" + B + "*>(p), p};\n}\n\n");
        } else {
            o.w("static spp::Instance " + cn + "_create()\n{\n    auto* p = new " + shell +
                "(spp::Construct{}" + ctor_args + ");\n    return {static_cast<" + B + "*>(p), p};\n}\n\n");
        }
        registers.push_back({cn, true});
    }

    o.w("extern \"C\" void script_register(spp::Registry& reg)\n{\n");
    for (const auto& r : registers) o.w("    reg.add(&" + r.first + "_record, &" + r.first + "_create);\n");
    for (const auto& t : thunk_regs) o.w(t);
    o.w("}\n");

    res.cpp = o.s;
    std::ostringstream mf;
    for (const auto& e : new_manifest) mf << e.cls << "::" << e.fn << " " << e.gen << "\n";
    res.slots_out = mf.str();
}

}  // namespace

namespace sppc {

TranslateResult translate(const TranslateOptions& in)
{
    TranslatorSession session;
    return translate(in, session);
}

TranslateResult translate(const TranslateOptions& in, TranslatorSession& session)
{
    TranslateResult res;
    g_diag.clear();
    g_script = in.script_name;
    try {
        translate_impl(in, *session.impl_, res);
        res.ok = true;
    } catch (const TranslateAbort&) {
        res.ok = false;
        res.cpp.clear();
        res.slots_out.clear();
    }
    res.diagnostics = g_diag.format(g_script);
    res.warnings = g_diag.format_warnings(g_script);
    return res;
}

}  // namespace sppc
