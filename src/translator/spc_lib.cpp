// spc_lib: the translator from script text to the text of a generation .cpp file.
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
// The function spc::translate takes strings and returns strings. It does not
// write a file. libclang reads the imported headers from the include dirs.
// The command line tool spc (src/translator/spc.cpp) is a thin file wrapper.
//
// The slot file keeps the slot order of the script functions stable between the
// generations. Native virtuals come first, in the order of the base class.
// The script functions follow. A function keeps its slot in all later generations.
#include "spp/spc_lib.h"

#include "ast.h"
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

using namespace spc::ast;

// State of the call in progress. Each thread has its own copy.
thread_local std::string g_script;  // the script name, as given by the caller
thread_local spc::Diagnostics g_diag;  // the diagnostics of the call

struct TranslateAbort {};

[[noreturn]] void fail(int line, const std::string& msg)
{
    g_diag.add_line(line, msg);
    throw TranslateAbort{};
}

// An error at a byte offset of the script. The diagnostic has a line and a column.
[[noreturn]] void fail_at(const spc::LineMap& map, uint32_t pos, const std::string& msg)
{
    g_diag.add(pos, map, msg);
    throw TranslateAbort{};
}

// Parses the script: the structure, the method bodies and the field initializers.
// The parser reports all syntax errors in source order. Any error stops the translation.
Script parse_structure(const std::string& text, const spc::LineMap& map)
{
    Script sc = spc::parse_script(text, map, g_diag);
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

struct NativeVirtual {
    std::string name, ret, owner;
    std::vector<std::string> ptypes;
    bool is_const = false, pure = false, final_ = false, no_except = false;
    std::string key() const
    {
        std::string k = name + "(";
        for (const auto& p : ptypes) k += norm(p) + ",";
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

using spc::RCtx;

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
void desugar_props(ClassDef& cd, const spc::LineMap& map)
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
struct FileStamp {
    std::string path;
    long long mtime_ns = -1;
    long long size = -1;
    bool operator==(const FileStamp& o) const
    {
        return path == o.path && mtime_ns == o.mtime_ns && size == o.size;
    }
};

FileStamp stamp_of(const std::string& path)
{
    FileStamp f;
    f.path = path;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        f.mtime_ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
        f.size = static_cast<long long>(st.st_size);
    }
    return f;
}

// Parses the imports with libclang. Reads the base classes in "names" that it finds.
// Gives the stamps of all files that the parse included.
void parse_bases(const std::string& probe, const std::vector<std::string>& args,
                 const std::set<std::string>& names, std::map<std::string, BaseInfo>& out,
                 std::vector<FileStamp>& files, Hierarchy& hierarchy)
{
    std::vector<const char*> cargs;
    for (const auto& a : args) cargs.push_back(a.c_str());
    ClangHandles ch;
    CXIndex idx = ch.idx = clang_createIndex(0, 0);
    CXUnsavedFile unsaved{"spc_probe.cpp", probe.c_str(), static_cast<unsigned long>(probe.size())};
    CXTranslationUnit tu = nullptr;
    CXErrorCode ec = clang_parseTranslationUnit2(idx, "spc_probe.cpp", cargs.data(),
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

    CXCursor root = clang_getTranslationUnitCursor(tu);
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

namespace spc {

// The cache of one key (flags + imports): the base classes and the files that the parse read.
struct TranslatorSession::Impl {
    struct Entry {
        std::vector<FileStamp> files;
        std::map<std::string, BaseInfo> bases;
        Hierarchy hierarchy;
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

}  // namespace spc

namespace {

// Gives the base classes for one key. A cache hit needs all names in the entry and
// the same stamp for each file. Otherwise it parses again and keeps the new result.
std::map<std::string, BaseInfo> session_bases(
    spc::TranslatorSession::Impl& S, const std::string& key, const std::set<std::string>& names,
    const std::function<void(std::map<std::string, BaseInfo>&, std::vector<FileStamp>&, Hierarchy&)>& parse,
    Hierarchy& hierarchy)
{
    std::lock_guard<std::mutex> lock(S.mutex);
    auto& e = S.entries[key];
    bool fresh = !e.files.empty();
    for (size_t i = 0; fresh && i < e.files.size(); ++i) fresh = stamp_of(e.files[i].path) == e.files[i];
    if (!fresh) e = {};
    bool complete = fresh;
    for (const auto& n : names) complete = complete && e.bases.count(n);
    if (!complete) {
        std::map<std::string, BaseInfo> out = std::move(e.bases);
        std::vector<FileStamp> files;
        Hierarchy hier;
        ++S.parses;
        try {
            parse(out, files, hier);
        } catch (...) {
            S.entries.erase(key);
            throw;
        }
        e.bases = std::move(out);
        e.files = std::move(files);
        e.hierarchy = std::move(hier);
    }
    hierarchy = e.hierarchy;
    std::map<std::string, BaseInfo> r;
    for (const auto& n : names) {
        auto it = e.bases.find(n);
        if (it != e.bases.end()) r.emplace(n, it->second);
    }
    return r;
}

void translate_impl(const spc::TranslateOptions& in, spc::TranslatorSession::Impl& session,
                    spc::TranslateResult& res)
{
    Options opt;
    opt.input = in.script_name;
    opt.output = in.output_name;
    opt.gen = in.generation;
    opt.includes = in.include_dirs;
    opt.resource_dir = in.resource_dir;
#ifdef SPC_RESOURCE_DIR
    if (opt.resource_dir.empty()) opt.resource_dir = SPC_RESOURCE_DIR;
#endif
    if (opt.gen < 1) fail(0, "the generation must be 1 or more");
    if (opt.output.empty()) opt.output = opt.input + ".cpp";
    g_script = opt.input;

    const spc::LineMap map(in.script_text);
    Script sc = parse_structure(in.script_text, map);

    // ---- the type model: the preludes first, then the stubs of the script
    spc::TypeModel types;
    for (const auto& [pname, ptext] : in.preludes) {
        const spc::LineMap pmap(ptext);
        spc::Diagnostics pd;
        Script ps = spc::parse_script(ptext, pmap, pd);
        if (pd.items.empty() && !ps.classes.empty()) {
            const ClassDef& pc = ps.classes.front();
            pd.add(pc.pos, pmap, "a prelude can hold only stubs: '" + pname + "' has the class '" + pc.name + "'");
        }
        if (pd.items.empty()) types.add_stubs(ps.stubs, pd, pmap);
        if (!pd.items.empty()) {
            for (auto& d : pd.items) {
                d.file = pname;
                g_diag.items.push_back(std::move(d));
            }
            throw TranslateAbort{};
        }
    }
    types.add_stubs(sc.stubs, g_diag, map);
    if (!g_diag.items.empty()) throw TranslateAbort{};
    for (const ClassDef& cd : sc.classes)
        if (cd.is_value) fail_at(map, cd.pos, "[value] on a script class is not supported yet");

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
    const std::map<std::string, BaseInfo> bases =
        session_bases(session, key, base_names,
                      [&](std::map<std::string, BaseInfo>& out, std::vector<FileStamp>& files,
                          Hierarchy& h) { parse_bases(probe, args, base_names, out, files, h); },
                      hier);
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
    o.w("// Generated by spc from " + opt.input + " (generation " + std::to_string(opt.gen) +
        "). Do not edit.\n");
    o.w("#include <cstddef>\n#include <cstdint>\n#include <cstdio>\n"
        "#include <new>\n#include <string>\n#include <type_traits>\n#include <typeinfo>\n"
        "#include <utility>\n\n#include \"spp/script_runtime.h\"\n\n");
    auto reset = [&] { o.w("#line " + std::to_string(o.lines + 2) + " \"" + esc(opt.output) + "\"\n"); };
    for (const auto& im : sc.imports) {
        o.w("#line " + std::to_string(im.line) + " \"" + esc(opt.input) + "\"\n");
        o.w("#include " + im.target() + "\n");
    }
    reset();
    o.w("\nconstexpr uint32_t GEN = " + std::to_string(opt.gen) + ";\n");

    std::vector<std::pair<std::string, bool>> registers;  // class name, for the register function
    std::vector<std::string> thunk_regs;

    for (ClassDef& cd : sc.classes) {
        const std::string cn = cd.name;
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
        else if (!bi.default_ctor)
            fail_at(map, cd.pos, "base class '" + cd.base + "' has no default constructor and no constructor that takes a name");

        // ---- check the script members
        desugar_props(cd, map);
        std::set<std::string> field_names;
        for (const Field& f : cd.fields)
            if (!field_names.insert(f.name).second) fail_at(map, f.pos, "field '" + f.name + "' is declared twice");
        std::set<std::string> fn_names;
        std::map<std::string, const Method*> overrides;  // by native key
        for (Method& m : cd.methods) {
            for (const Param& p : m.params)
                if (p.name == "self" || p.name == "st" || p.name == "a")
                    fail_at(map, m.pos, "parameter name '" + p.name + "' is reserved");
            // A method overrides a base virtual method when the name and the parameter types match.
            // The method must have the modifier "override" (C# rules).
            std::string key = m.name + "(";
            for (const Param& p : m.params) key += norm(p.type) + ",";
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
            if (!m.ret.empty() && norm(m.ret) != norm(hit->ret))
                fail_at(map, m.pos, "return type '" + m.ret + "' differs from '" + hit->ret + "'");
            if (!overrides.emplace(hit->key(), &m).second)
                fail_at(map, m.pos, "'" + hit->text() + "' is overridden twice");
        }
        for (const auto& nv : bi.virtuals)
            if (nv.pure && !overrides.count(nv.key()))
                fail_at(map, cd.pos, "class " + cn + " does not override the pure virtual method '" +
                                  nv.text() + "' of " + nv.owner);

        // ---- the C++ storage types. An override keeps the native types.
        for (Field& f : cd.fields) f.type = types.storage(f.type);
        for (Method& m : cd.methods) {
            if (m.overrides_virtual) continue;
            for (Param& p : m.params) p.type = types.param(p.type);
            if (!m.ret.empty()) m.ret = types.storage(m.ret);
        }

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
            if (!m.overrides_virtual) fn_by_name[m.name] = &m;
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
            if (m.overrides_virtual) continue;
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
        RCtx base_cx = spc::make_class_ctx(cd);
        base_cx.map = &map;
        base_cx.types = &types;
        for (const auto& sc_cls : sc.classes) base_cx.script_classes.insert(sc_cls.name);

        const std::string S = cn + "State_g" + std::to_string(opt.gen);
        const std::string shell = cn + "_Shell";
        const std::string B = cd.base;

        o.w("\n// ---------------------------------------------------------------- class " + cn + "\n");
        o.w("// Slot numbers. Native virtuals come first, in the order of " + B + ".\n");
        o.w("enum : unsigned {\n");
        for (size_t i = 0; i < slots.size(); ++i)
            o.w("    " + slot_id(slots[i].id) + " = " + std::to_string(i) + ",\n");
        o.w("    " + slot_id("COUNT") + " = " + std::to_string(slots.size()) + ",\n};\n\n");
        o.w("extern script::ClassRecord " + cn + "_record;\n\n");

        // ---- shell
        o.w("// The shell overrides every virtual of " + B + ". Each stub reads one slot.\n");
        o.w("struct " + shell + " final : " + B + ", script::ShellBase {\n");
        o.w("    template <class... A>\n    explicit " + shell + "(script::Construct, A&&... a) : " + B +
            "(std::forward<A>(a)...)\n    {\n        script::init_state(this, &" + cn +
            "_record);\n    }\n    ~" + shell + "() override { script::destroy_state(this); }\n");
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
            o.w("        return script::call<" + nv.ret + "(" + str_join(sig, ", ") + ")>(cls, " +
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
                spc::lower_expr(f.init, fc, g_diag);
                if (!g_diag.items.empty()) throw TranslateAbort{};
                init = spc::ast::print(*f.init);
            }
            o.w("#line " + std::to_string(f.line) + " \"" + esc(opt.input) + "\"\n");
            o.w("static void " + P + "init_" + f.name + "(void*, void* f) { using T = " + f.type +
                "; new (f) T(" + init + "); }\n");
            reset();
        }
        o.w("\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n");
        if (cd.fields.empty()) {
            o.w("static const script::FieldInfo* const " + cn + "_fields = nullptr;\n");
        } else {
            o.w("static const script::FieldInfo " + cn + "_fields[] = {\n");
            for (const Field& f : cd.fields) {
                const std::string T = f.type;
                o.w("    {\"" + f.name + "\", " + (f.alias.empty() ? "nullptr" : "\"" + f.alias + "\"") +
                    ", typeid(" + T + ").name(), offsetof(" + S + ", " + f.name + "), sizeof(" + T +
                    "),\n     alignof(" + T + "), script::field_flags<" + T + ">(), " + P + "init_" +
                    f.name + ", script::move_field<" + T + ">, script::field_destroyer<" + T +
                    ">()},\n");
            }
            o.w("};\n");
        }
        o.w("#pragma clang diagnostic pop\n\n");
        o.w("static const script::StateLayout " + cn + "_layout = {\n    \"" + S + "\", sizeof(" + S +
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
            if (!m.overrides_virtual)
                o.w("static " + fn_ret(m) + " " + P + m.name + "(" + shell + "* self" + param_decl(m) + ");\n");
        o.w("\n");

        auto emit_body = [&](Method& m, bool is_fn, const std::string& sym, const std::string& ret,
                             const std::string& what) {
            RCtx cx = base_cx;
            cx.where = what;
            spc::add_params(cx, m.params);
            std::string head = "static " + ret + " " + sym + "(";
            if (is_fn) head += shell + "* self" + param_decl(m);
            else head += B + "* a" + param_decl(m);
            o.w(head + ")\n{\n");
            if (!is_fn) o.w("    [[maybe_unused]] auto* self = static_cast<" + shell + "*>(a);\n");
            o.w("    if (SCRIPT_UNLIKELY(script::active_generation != GEN)) {\n");
            o.w("        script::stale_call(\"" + what + "\");\n        return" +
                (ret == "void" ? std::string() : " " + ret + "()") + ";\n    }\n");
            o.w("    [[maybe_unused]] auto* st = state_of(self);\n");
            spc::lower_block(m.body, cx, g_diag);
            if (!g_diag.items.empty()) throw TranslateAbort{};
            spc::ast::Emitter em(map, opt.input);
            spc::ast::emit_stmts(m.body.stmts, em, 1);
            em.finish();
            o.w(em.text());
            reset();
            o.w("}\n\n");
        };

        for (Method& m : cd.methods) {
            if (m.overrides_virtual) {
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
            o.w("    return script::call<" + fn_ret(m) + "(" + B + "*" + type_list(m) + ")>(self->cls, " +
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
        o.w("script::ClassRecord " + cn + "_record = {\n    \"" + cn + "\", GEN, " + cn + "_slots, " +
            slot_id("COUNT") + ", &" + cn + "_layout, nullptr,\n    \"" + B + "\", script::shell_layout_hash(\"" +
            B + "\", sizeof(" + shell + "), alignof(" + shell + "), " + std::to_string(native_count) +
            "),\n};\n\n");
        o.w("static script::Instance " + cn + "_create()\n{\n    auto* p = new " + shell +
            "(script::Construct{}" + ctor_args + ");\n    return {static_cast<" + B + "*>(p), p};\n}\n\n");
        registers.push_back({cn, true});
    }

    o.w("extern \"C\" void script_register(script::Registry& reg)\n{\n");
    for (const auto& r : registers) o.w("    reg.add(&" + r.first + "_record, &" + r.first + "_create);\n");
    for (const auto& t : thunk_regs) o.w(t);
    o.w("}\n");

    res.cpp = o.s;
    std::ostringstream mf;
    for (const auto& e : new_manifest) mf << e.cls << "::" << e.fn << " " << e.gen << "\n";
    res.slots_out = mf.str();
}

}  // namespace

namespace spc {

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
    return res;
}

}  // namespace spc
