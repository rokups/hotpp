// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "hot/translate_cache.h"

#include "cache_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace hot
{

namespace fs = std::filesystem;
using namespace cache_io;

namespace
{

constexpr int entry_version = 2;
constexpr uint64_t second_seed = 0x9e3779b97f4a7c15ULL;

// ------------------------------------------------------------ The fields of TranslateOptions

// The number of the fields of an aggregate: the largest count of initializers that compile.
struct AnyField
{
    template <class T>
    operator T() const;
};

template <class T, class... A>
constexpr size_t field_count(long)
{
    return sizeof...(A);
}

template <class T, class... A>
constexpr auto field_count(int) -> decltype(T{std::declval<A>()..., std::declval<AnyField>()}, size_t{})
{
    return field_count<T, A..., AnyField>(0);
}

// A new field of TranslateOptions needs a decision: add it to add_options() when it can change
// the output, or tell here why not. Then change the number.
//   - slots_in: not in the key. The manifest has the digest of the lines that the output reads.
//   - fs: only "is there a file system" is in the key. The manifest checks the files.
static_assert(field_count<hotc::TranslateOptions>(0) == 21,
    "TranslateOptions has a new field: decide if it goes into the key of the translate cache");

void add_list(Hash64& h, const std::vector<std::string>& v)
{
    h.add_u64(v.size());
    for (const std::string& s : v)
        h.add_text(s);
}

void add_pairs(Hash64& h, const std::vector<std::pair<std::string, std::string>>& v)
{
    h.add_u64(v.size());
    for (const auto& [a, b] : v)
    {
        h.add_text(a);
        h.add_text(b);
    }
}

void add_options(Hash64& h, const hotc::TranslateOptions& o, const std::string& build_id, const std::string& extra,
    const std::string& cwd)
{
    h.add_text("hot-translate-cache");
    h.add_u64(entry_version);
    h.add_text(build_id);
    h.add_text(extra);
    h.add_text(cwd); // the relative paths of the options start here
    h.add_text(o.script_text);
    h.add_text(o.script_name);
    h.add_text(o.unit_name);
    h.add_text(o.output_name);
    h.add_u64(static_cast<uint64_t>(static_cast<int64_t>(o.generation)));
    add_list(h, o.include_dirs);
    h.add_text(o.resource_dir);
    h.add_text(o.target);
    h.add_text(o.sysroot);
    // The defaults of the build for an empty target or sysroot.
    add_list(h, hotc::target_parse_args(o.target, o.sysroot));
    h.add_text(o.std_version);
    add_list(h, o.defines);
    add_pairs(h, o.preludes);
    add_pairs(h, o.modules);
    h.add_u64(o.strict_borrowed);
    h.add_u64(o.null_warnings);
    h.add_text(o.format_lib);
    h.add_text(o.view_type);
    h.add_u64(o.fs != nullptr);
    h.add_u64(o.aot);
    h.add_u64(o.aot_optimized);
}

// ------------------------------------------------------------ JSON

std::string hex64(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string hash_hex(const std::string& text)
{
    return hex64(hotc::text_hash(text));
}

class Writer
{
public:
    std::string out;

    void begin(char c) { sep(); out.push_back(c); first_ = true; }
    void end(char c) { out.push_back(c); first_ = false; }
    void key(const char* name)
    {
        sep();
        append_json_string(out, name);
        out.push_back(':');
        first_ = true; // the value has no comma in front
    }
    void str(const std::string& s) { sep(); append_json_string(out, s); }
    void num(int64_t n) { sep(); out += std::to_string(n); }
    void field(const char* name, const std::string& s) { key(name); str(s); }
    void field(const char* name, int64_t n) { key(name); num(n); }
    void list(const char* name, const std::vector<std::string>& v)
    {
        key(name);
        begin('[');
        for (const std::string& s : v)
            str(s);
        end(']');
    }

private:
    void sep()
    {
        if (!first_)
            out.push_back(',');
        first_ = false;
    }
    bool first_ = true;
};

const std::string* get_str(const Json& j, const char* name)
{
    const Json* v = j.find(name);
    return v && v->type == Json::Type::String ? &v->text : nullptr;
}

const Json* get_array(const Json& j, const char* name)
{
    const Json* v = j.find(name);
    return v && v->type == Json::Type::Array ? v : nullptr;
}

bool get_num(const Json& j, const char* name, int64_t& out)
{
    const Json* v = j.find(name);
    if (!v || v->type != Json::Type::Number)
        return false;
    out = v->number;
    return true;
}

bool get_list(const Json& j, const char* name, std::vector<std::string>& out)
{
    const Json* a = get_array(j, name);
    if (!a)
        return false;
    for (const Json& i : a->items)
    {
        if (i.type != Json::Type::String)
            return false;
        out.push_back(i.text);
    }
    return true;
}

bool parse_hex64(const std::string& s, uint64_t& out)
{
    if (s.size() != 16)
        return false;
    char* end = nullptr;
    out = std::strtoull(s.c_str(), &end, 16);
    return end == s.c_str() + s.size();
}

// The time and the size of a disk file.
bool disk_stamp(const std::string& path, int64_t& mtime, int64_t& size)
{
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(fs::path(path), ec);
    if (ec)
        return false;
    const uintmax_t s = fs::file_size(fs::path(path), ec);
    if (ec)
        return false;
    mtime = to_ns(t);
    size = static_cast<int64_t>(s);
    return true;
}

// The parts of the slot output that belong to the classes of the script (hotc::SlotCarry).
struct OwnSlots
{
    std::string manifest, sigs, fields;
};

bool split_slots(const hotc::SlotCarry& carry, const hotc::TranslateResult& r, OwnSlots& own)
{
    if (r.slots_out.compare(0, carry.manifest.size(), carry.manifest) != 0)
        return false;
    if (r.sigs_out.compare(0, carry.sigs.size(), carry.sigs) != 0)
        return false;
    own.manifest = r.slots_out.substr(carry.manifest.size());
    const std::string rest = r.sigs_out.substr(carry.sigs.size());
    size_t pos = 0;
    while (pos < rest.size() && rest.compare(pos, 4, "sig ") == 0)
    {
        const size_t nl = rest.find('\n', pos);
        pos = nl == std::string::npos ? rest.size() : nl + 1;
    }
    own.sigs = rest.substr(0, pos);
    if (rest.compare(pos, carry.fields.size(), carry.fields) != 0)
        return false;
    own.fields = rest.substr(pos + carry.fields.size());
    // The parts must give the output again, else the entry is not safe.
    return carry.manifest + own.manifest == r.slots_out
        && carry.sigs + own.sigs + carry.fields + own.fields == r.sigs_out;
}

bool ends_with(const std::string& s, const char* tail)
{
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

} // namespace

TranslateCache::TranslateCache(fs::path dir, std::string build_id, std::string extra)
    : dir_(std::move(dir))
    , build_id_(build_id.empty() ? std::string(hotc::translator_build_id()) : std::move(build_id))
    , extra_(std::move(extra))
{
}

bool TranslateCache::enabled_by_env()
{
    for (const char* name : {"HOT_CACHE", "HOT_TRANSLATE_CACHE"})
        if (const char* v = std::getenv(name); v && std::strcmp(v, "0") == 0)
            return false;
    return true;
}

ObjectCache::Key TranslateCache::make_key(const hotc::TranslateOptions& options) const
{
    std::error_code ec;
    const std::string cwd = fs::current_path(ec).generic_string();
    Hash64 a;
    Hash64 b(second_seed);
    add_options(a, options, build_id_, extra_, cwd);
    add_options(b, options, build_id_, extra_, cwd);
    return {a.value(), b.value()};
}

fs::path TranslateCache::entry_file(const ObjectCache::Key& key, const char* ext) const
{
    return dir_ / "tr" / (key.hex() + ext);
}

void TranslateCache::remove(const ObjectCache::Key& key)
{
    std::error_code ec;
    fs::remove(entry_file(key, ".json"), ec);
    fs::remove(entry_file(key, ".cpp"), ec);
    fs::remove(entry_file(key, ".data"), ec);
}

std::optional<hotc::TranslateResult> TranslateCache::lookup(const hotc::TranslateOptions& options)
{
    std::optional<hotc::TranslateResult> r;
    try
    {
        r = lookup_entry(options);
    }
    catch (...)
    {
        r.reset();
    }
    if (r)
        ++hits_;
    else
        ++misses_;
    return r;
}

std::optional<hotc::TranslateResult> TranslateCache::lookup_entry(const hotc::TranslateOptions& o)
{
    const ObjectCache::Key key = make_key(o);
    const fs::path manifest = entry_file(key, ".json");
    std::string text;
    Json m;
    if (!read_file(manifest, text) || !parse_json(text, m) || m.type != Json::Type::Object)
        return std::nullopt;
    int64_t version = 0;
    const std::string* hex = get_str(m, "key");
    if (!get_num(m, "version", version) || version != entry_version || !hex || *hex != key.hex())
        return std::nullopt;

    // The slot lines of the classes: the same lines give the same output.
    std::vector<std::string> classes, keys;
    const std::string* slots = get_str(m, "slots");
    if (!get_list(m, "classes", classes) || !get_list(m, "keys", keys) || !slots)
        return std::nullopt;
    const hotc::SlotCarry carry = hotc::carry_slots(o.slots_in, classes, keys, o.generation);
    if (hash_hex(carry.read) != *slots)
        return std::nullopt;

    // Each search for an imported script finds the same file with the same text.
    std::map<std::string, std::string> import_text;
    std::vector<hotc::ImportLookup> lookups;
    const Json* imports = get_array(m, "imports");
    if (!imports)
        return std::nullopt;
    for (const Json& i : imports->items)
    {
        hotc::ImportLookup l;
        const std::string* path = get_str(i, "path");
        const std::string* from_dir = get_str(i, "from_dir");
        const std::string* from_unit_dir = get_str(i, "from_unit_dir");
        const std::string* file = get_str(i, "file");
        const std::string* hash = get_str(i, "hash");
        if (!path || !from_dir || !from_unit_dir || !file || !hash || !parse_hex64(*hash, l.text_hash))
            return std::nullopt;
        std::string found_text, found_file, unit;
        if (!hotc::find_import(o, *path, *from_dir, *from_unit_dir, found_text, found_file, unit)
            || found_file != *file || hotc::text_hash(found_text) != l.text_hash)
            return std::nullopt;
        l.path = *path;
        l.from_dir = *from_dir;
        l.from_unit_dir = *from_unit_dir;
        l.file = *file;
        import_text[*file] = std::move(found_text);
        lookups.push_back(std::move(l));
    }

    // The headers of the host file system have the same text.
    std::vector<std::pair<std::string, std::string>> virtual_files;
    std::set<std::string> virtual_paths;
    const Json* virtuals = get_array(m, "virtual");
    if (!virtuals)
        return std::nullopt;
    for (const Json& v : virtuals->items)
    {
        const std::string* path = get_str(v, "path");
        const std::string* hash = get_str(v, "hash");
        std::string content;
        if (!path || !hash || !o.fs || !o.fs->read(*path, content) || hash_hex(content) != *hash)
            return std::nullopt;
        virtual_paths.insert(*path);
        virtual_files.emplace_back(*path, std::move(content));
    }

    // The disk headers have the same stamps.
    const Json* deps = get_array(m, "deps");
    if (!deps)
        return std::nullopt;
    for (const Json& d : deps->items)
    {
        const std::string* path = get_str(d, "path");
        const std::string* kind = get_str(d, "kind");
        if (!path || !kind)
            return std::nullopt;
        if (*kind == "import")
        {
            if (!import_text.count(*path))
                return std::nullopt;
            continue;
        }
        if (*kind == "vfs")
        {
            if (!virtual_paths.count(*path))
                return std::nullopt;
            continue;
        }
        int64_t mtime = 0, size = 0, have_mtime = 0, have_size = 0;
        if (*kind != "disk" || !get_num(d, "mtime", mtime) || !get_num(d, "size", size))
            return std::nullopt;
        // A file of the host file system now hides the disk file.
        if (o.fs && o.fs->stat(*path).exists)
            return std::nullopt;
        if (!disk_stamp(*path, have_mtime, have_size) || have_mtime != mtime || have_size != size)
            return std::nullopt;
    }

    // No file is at a shadow path: the header search finds the same headers.
    std::vector<std::string> shadows;
    if (!get_list(m, "shadows", shadows))
        return std::nullopt;
    for (const std::string& s : shadows)
        if (hotc::shadow_exists(o.fs.get(), s))
            return std::nullopt;

    // The payload files have the hashes of the store.
    std::string cpp, data_text;
    Json data;
    const std::string* cpp_hash = get_str(m, "cpp_hash");
    const std::string* data_hash = get_str(m, "data_hash");
    if (!cpp_hash || !data_hash || !read_file(entry_file(key, ".cpp"), cpp) || hash_hex(cpp) != *cpp_hash
        || !read_file(entry_file(key, ".data"), data_text) || hash_hex(data_text) != *data_hash
        || !parse_json(data_text, data) || data.type != Json::Type::Object)
        return std::nullopt;

    hotc::TranslateResult r;
    const std::string* unit = get_str(data, "unit");
    const std::string* warnings = get_str(data, "warnings");
    const std::string* iface = get_str(data, "iface");
    const std::string* iface_hash = get_str(data, "iface_hash");
    const std::string* iface_header = get_str(data, "iface_header");
    const std::string* own_manifest = get_str(data, "own_manifest");
    const std::string* own_sigs = get_str(data, "own_sigs");
    const std::string* own_fields = get_str(data, "own_fields");
    std::vector<std::string> import_files;
    if (!unit || !warnings || !iface || !iface_hash || !parse_hex64(*iface_hash, r.iface_hash) || !iface_header
        || !own_manifest || !own_sigs || !own_fields || !get_list(data, "dependencies", r.dependencies)
        || !get_list(data, "imports", import_files))
        return std::nullopt;
    const Json* import_hashes = get_array(data, "import_hashes");
    const Json* segments = get_array(data, "segments");
    const Json* generic_names = get_array(data, "generic_names");
    if (!import_hashes || !segments || !generic_names || segments->items.size() % 5 != 0
        || generic_names->items.size() % 2 != 0)
        return std::nullopt;
    for (const Json& h : import_hashes->items)
    {
        const std::string* u = get_str(h, "unit");
        const std::string* v = get_str(h, "hash");
        uint64_t value = 0;
        if (!u || !v || !parse_hex64(*v, value))
            return std::nullopt;
        r.import_hashes.emplace_back(*u, value);
    }
    for (const std::string& f : import_files)
    {
        auto it = import_text.find(f);
        if (it == import_text.end())
            return std::nullopt;
        r.imports.emplace_back(f, it->second);
        r.source_map.add_file(f, it->second);
    }
    for (size_t i = 0; i < segments->items.size(); i += 5)
    {
        uint32_t v[5];
        for (size_t k = 0; k < 5; ++k)
        {
            const Json& n = segments->items[i + k];
            if (n.type != Json::Type::Number || n.number < 0 || n.number > 0xffffffffLL)
                return std::nullopt;
            v[k] = static_cast<uint32_t>(n.number);
        }
        r.source_map.add({v[0], v[1], v[2], v[3], v[4]});
    }
    hotc::SourceMap::GenericNames names;
    for (size_t i = 0; i < generic_names->items.size(); i += 2)
    {
        const Json& a = generic_names->items[i];
        const Json& b = generic_names->items[i + 1];
        if (a.type != Json::Type::String || b.type != Json::Type::String)
            return std::nullopt;
        names.emplace_back(a.text, b.text);
    }
    r.source_map.set_generic_names(std::move(names));
    r.source_map.build(o.script_text, cpp);

    r.ok = true;
    r.cpp = std::move(cpp);
    r.slots_out = carry.manifest + *own_manifest;
    r.sigs_out = carry.sigs + *own_sigs + carry.fields + *own_fields;
    r.warnings = *warnings;
    r.virtual_files = std::move(virtual_files);
    r.iface = *iface;
    r.unit = *unit;
    r.iface_header = *iface_header;
    r.import_lookups = std::move(lookups);
    r.slot_classes = std::move(classes);
    r.slot_keys = std::move(keys);
    r.shadow_paths = std::move(shadows);

    // The time of the manifest is the age of the entry for ObjectCache::trim.
    std::error_code touch_ec;
    fs::last_write_time(manifest, fs::file_time_type::clock::now(), touch_ec);
    return r;
}

bool TranslateCache::store(const hotc::TranslateOptions& o, const hotc::TranslateResult& r,
    fs::file_time_type started, std::string& error)
{
    try
    {
        if (!r.ok || !r.diagnostics.empty())
        {
            error = "the translate has errors";
            return false;
        }
        // The front end of a ".cpp" script parses its own includes. Its list of dependencies is
        // not known to be complete, so it gets no entry.
        if (ends_with(o.script_name, ".cpp"))
        {
            error = "a .cpp script has no entry";
            return false;
        }
        const hotc::SlotCarry carry = hotc::carry_slots(o.slots_in, r.slot_classes, r.slot_keys, o.generation);
        OwnSlots own;
        if (!split_slots(carry, r, own))
        {
            error = "the slot output does not split";
            return false;
        }
        // Each imported script has a search with the hash of its text.
        std::map<std::string, uint64_t> lookup_hash;
        for (const hotc::ImportLookup& l : r.import_lookups)
            lookup_hash[l.file] = l.text_hash;
        std::set<std::string> import_files;
        for (const auto& [file, text] : r.imports)
        {
            auto it = lookup_hash.find(file);
            if (it == lookup_hash.end() || it->second != hotc::text_hash(text))
            {
                error = "the import '" + file + "' has no search";
                return false;
            }
            import_files.insert(file);
        }
        // The source map has the imported scripts as files, in the same order.
        const std::vector<hotc::SourceFile>& map_files = r.source_map.files();
        bool same_files = map_files.size() == r.imports.size();
        for (size_t i = 0; same_files && i < map_files.size(); ++i)
            same_files = map_files[i].name == r.imports[i].first && map_files[i].text == r.imports[i].second;
        if (!same_files)
        {
            error = "the source map has other files than the imports";
            return false;
        }

        Writer man;
        man.begin('{');
        man.field("version", entry_version);
        man.field("key", make_key(o).hex());
        man.list("classes", r.slot_classes);
        man.list("keys", r.slot_keys);
        man.field("slots", hash_hex(carry.read));
        man.key("imports");
        man.begin('[');
        for (const hotc::ImportLookup& l : r.import_lookups)
        {
            man.begin('{');
            man.field("path", l.path);
            man.field("from_dir", l.from_dir);
            man.field("from_unit_dir", l.from_unit_dir);
            man.field("file", l.file);
            man.field("hash", hex64(l.text_hash));
            man.end('}');
        }
        man.end(']');
        std::set<std::string> virtual_paths;
        man.key("virtual");
        man.begin('[');
        for (const auto& [path, content] : r.virtual_files)
        {
            virtual_paths.insert(path);
            man.begin('{');
            man.field("path", path);
            man.field("hash", hash_hex(content));
            man.end('}');
        }
        man.end(']');
        // A header that changed after the start of the parse (or near it, with a coarse file
        // time) can differ from the text of the parse. A session entry can come from a parse of
        // an earlier translate, so the earlier time of the two starts applies.
        int64_t start_ns = to_ns(started);
        if (r.parse_start_ns != 0)
            start_ns = std::min(start_ns, r.parse_start_ns);
        const int64_t newest = start_ns - int64_t(2000000000);
        man.key("deps");
        man.begin('[');
        for (const std::string& d : r.dependencies)
        {
            man.begin('{');
            man.field("path", d);
            if (import_files.count(d))
                man.field("kind", "import");
            else if (virtual_paths.count(d))
                man.field("kind", "vfs");
            else
            {
                if (o.fs && o.fs->stat(d).exists)
                {
                    error = "the host file '" + d + "' is not in the virtual files";
                    return false;
                }
                int64_t mtime = 0, size = 0;
                if (!disk_stamp(d, mtime, size))
                {
                    error = "cannot stat '" + d + "'";
                    return false;
                }
                if (mtime > newest)
                {
                    error = "the header '" + d + "' changed during the translate";
                    return false;
                }
                man.field("kind", "disk");
                man.field("mtime", mtime);
                man.field("size", size);
            }
            man.end('}');
        }
        man.end(']');
        // A file at a shadow path now hides a header of the parse.
        for (const std::string& s : r.shadow_paths)
            if (hotc::shadow_exists(o.fs.get(), s))
            {
                error = "the file '" + s + "' hides a header of the parse";
                return false;
            }
        man.list("shadows", r.shadow_paths);

        Writer data;
        data.begin('{');
        data.field("unit", r.unit);
        data.field("warnings", r.warnings);
        data.field("iface", r.iface);
        data.field("iface_hash", hex64(r.iface_hash));
        data.field("iface_header", r.iface_header);
        data.field("own_manifest", own.manifest);
        data.field("own_sigs", own.sigs);
        data.field("own_fields", own.fields);
        data.list("dependencies", r.dependencies);
        std::vector<std::string> files;
        for (const auto& i : r.imports)
            files.push_back(i.first);
        data.list("imports", files);
        data.key("import_hashes");
        data.begin('[');
        for (const auto& [unit, hash] : r.import_hashes)
        {
            data.begin('{');
            data.field("unit", unit);
            data.field("hash", hex64(hash));
            data.end('}');
        }
        data.end(']');
        data.key("segments");
        data.begin('[');
        for (const hotc::SourceSegment& s : r.source_map.segments())
            for (const uint32_t v : {s.script_begin, s.script_len, s.gen_begin, s.gen_len, s.file})
                data.num(v);
        data.end(']');
        data.key("generic_names");
        data.begin('[');
        for (const auto& [a, b] : r.source_map.generic_names())
        {
            data.str(a);
            data.str(b);
        }
        data.end(']');
        data.end('}');

        man.field("cpp_hash", hash_hex(r.cpp));
        man.field("data_hash", hash_hex(data.out));
        man.end('}');

        const ObjectCache::Key key = make_key(o);
        std::error_code ec;
        fs::create_directories(dir_ / "tr", ec);
        if (ec)
        {
            error = "cannot create " + (dir_ / "tr").string() + ": " + ec.message();
            return false;
        }
        // The manifest comes last: an entry without it is a miss.
        if (!write_file_atomic(entry_file(key, ".cpp"), r.cpp, error)
            || !write_file_atomic(entry_file(key, ".data"), data.out, error)
            || !write_file_atomic(entry_file(key, ".json"), man.out, error))
            return false;
        return true;
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    catch (...)
    {
        error = "unknown error";
        return false;
    }
}

} // namespace hot
