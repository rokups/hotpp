// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The saved symbol index of hot-ls (S28). It is like the index of clangd.
//
// There is one shard for each .hot file. All shards are in one flat directory (by default
// ".cache/hot/index/" in the project root). The name of a shard is a hash of the file path. So
// a new shard replaces the old shard of the same file, and the index does not grow when a file
// changes.
//
// A shard has a header line and a JSON body. The header line is
// "hot-index <version> <digest> <path>". The body holds the symbols and the references.
// load() gives a shard only when the format version and the content digest match. A shard
// with another version is deleted. prune() deletes the shard of a missing file.
// The body (version 5) has the classes ("c") and the table of the resolved names ("t"). The
// table keeps offset, length, kind, is_decl, decl, decl_file, near_decl and near_file of each
// ref, the syntax errors and the base of each class. The strings are in a
// string table. load() computes ResolvedRef::id and the line and the column of each name.
// A shard with a table has the marker "tp" (IndexShard::has_table). load() deletes a shard
// without the marker, so a shard with a missing table never replaces an analyze.
// The digest covers the text of the file and the texts of the scripts that it imports.
// A write goes to a temporary file first. Then a rename puts it in place.
#pragma once

#include "ref_resolver.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace hot::ls
{

struct IndexPos
{
    int line = 0, col = 0; // 0-based, in bytes
};

struct IndexMember
{
    std::string name, kind;
    IndexPos begin;
    // The places of the member in this file. No value: not computed yet (this needs clangd).
    std::optional<std::vector<IndexPos>> refs;
};

struct IndexClass
{
    std::string name;
    IndexPos begin; // the place of the class name
    std::vector<IndexPos> refs;
    std::vector<IndexMember> members;
    IndexMember* find_member(const std::string& member);
};

// A place that clangd gave for a native name (HOT-35).
struct NativeTarget
{
    std::string uri;
    int line = 0, character = 0;
    bool operator==(const NativeTarget& o) const { return uri == o.uri && line == o.line && character == o.character; }
};

// The size and the time of a file. A missing file has size UINT64_MAX and mtime 0.
struct NativeStamp
{
    std::string path;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool operator==(const NativeStamp& o) const { return path == o.path && size == o.size && mtime == o.mtime; }
};

// The guard of the native answers of a script (HOT-35). "includes" are the include names of the shell
// header of the generated unit. "header_stamp" is the Server::header_stamp of these includes. "host_key"
// is the hash of the host options and of the clangd program. The answers hold only while both match.
struct NativeGuard
{
    std::vector<std::string> includes;
    uint64_t header_stamp = 0;
    uint64_t host_key = 0;
    bool operator==(const NativeGuard& o) const
    {
        return includes == o.includes && header_stamp == o.header_stamp && host_key == o.host_key;
    }
};

struct IndexShard
{
    std::vector<IndexClass> classes;
    // The answers of clangd for the native names of a closed script (HOT-35). The key is the
    // offset of the asked position. The value is the list of the targets. An empty list is an
    // answer too. "native_stamps" has one stamp for each target file. Version 5 of the format.
    std::map<int, std::vector<NativeTarget>> native_targets;
    std::vector<NativeStamp> native_stamps;
    NativeGuard native_guard;
    FileRefs table; // the resolved names of the file (from the translator)
    // True when "table" comes from an analyze with the resolved names. store() writes the marker.
    // load() gives only a shard with the marker.
    bool has_table = false;
    IndexClass* find_class(const std::string& name);
};

class SymbolIndex
{
public:
    static constexpr int kFormatVersion = 5;

    SymbolIndex();
    ~SymbolIndex();

    // Sets the directory. An empty directory switches the index off.
    void open(const std::string& dir);
    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    // The digest of a file content.
    static std::string digest(const std::string& content);
    // The shard digest of a script. "imports" is the result of cached_digest. "preludes" are the
    // (path, text) pairs of the prelude files that exist, in the order of the analysis. The
    // worker and the server both call it, so they share the shards.
    static std::string shard_digest(const std::string& imports,
        const std::vector<std::pair<std::string, std::string>>& preludes, const std::string& flags_text,
        const std::string& build_id);
    // The digest of a script and of the scripts that it imports ("using "x.hot";"), also the
    // imports of the imports. A script import is read from the directory of the script, then from
    // the include dirs. When an imported script changes, the digest changes.
    static std::string digest(
        const std::string& script_path, const std::string& content, const std::vector<std::string>& include_dirs);
    // Same as the digest above. It keeps the result for each script path. It reuses the result
    // when the content, the include dirs, and the size and the time of each imported file do not
    // change. The content is the text of the script (an open document text is possible).
    // "deps" (if given) gets the stamp of each file that the search probed, in the order of the
    // search. A probed file that is missing has size UINT64_MAX and mtime 0.
    struct DepStamp
    {
        std::string path;
        uint64_t size = 0;
        int64_t mtime = 0;
    };
    std::string cached_digest(const std::string& script_path, const std::string& content,
        const std::vector<std::string>& include_dirs, std::vector<DepStamp>* deps = nullptr);
    // The number of import files that the digest functions read from disk (a test hook).
    static long digest_file_reads();
    // The path of the shard file of a script path.
    std::string shard_path(const std::string& script_path) const;

    // Gives the shard of a script if the version and the digest match. Else gives no value.
    std::optional<IndexShard> load(const std::string& script_path, const std::string& digest);
    // Writes the shard of a script. It replaces the old shard. Gives false on an error.
    bool store(const std::string& script_path, const std::string& digest, const IndexShard& shard);
    // Adds the native answers to the shard of a script. It loads the shard, merges the answers
    // (an offset in "map" replaces the old offset) and stores the shard. When a stored stamp
    // differs from a new stamp of the same file, or the stored guard differs from "guard", it drops the old answers first. Gives false when
    // there is no shard with this digest or the store fails.
    bool store_native(const std::string& script_path, const std::string& digest,
        const std::map<int, std::vector<NativeTarget>>& map, const std::vector<NativeStamp>& stamps,
        const NativeGuard& guard);
    // The stamp of a file now.
    static NativeStamp stamp_file(const std::string& path);
    // Deletes the shards of missing files, the shards with another version and old temporary
    // files. Gives the number of deleted files.
    int prune();
    // Deletes the shards of the files for which "match" gives true. Gives the number of deleted
    // shards.
    int remove_if(const std::function<bool(const std::string&)>& match);

    int loads() const { return loads_.load(); }
    int stores() const { return stores_.load(); }
    // The total time of the good shard loads, in ms.
    double load_ms() const { return static_cast<double>(load_us_.load()) / 1000.0; }

private:
    struct DigestEntry;
    // The class is safe for many threads. A mutex guards the digest cache. Load and store take
    // the shard lock shared. Prune and remove_if take it exclusive.
    std::mutex digest_mutex_;
    std::shared_mutex shard_mutex_;
    std::map<std::string, std::shared_ptr<DigestEntry>> digest_cache_;
    std::string dir_;
    std::atomic<int> loads_{0}, stores_{0};
    std::atomic<long long> load_us_{0};
};

} // namespace hot::ls
