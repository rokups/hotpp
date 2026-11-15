// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The workspace index of hot-ls (ticket HOT-34). It keeps one immutable Entry for each script
// and the inverted maps over all entries: word to files, unclaimed word to files, and ref id to
// files. A later step lets the server and the background threads use it.
//
// This part has data and maps only. It starts no thread.
// The class is safe for many threads. One mutex guards the entries and the maps. No function
// holds the mutex during hotc::analyze, a SymbolIndex call or a file read. The lock order is:
// WorkspaceIndex::mutex_ first, then the locks of SymbolIndex.
#pragma once

#include "hot/hotc_lib.h"
#include "ref_resolver.h"
#include "symbol_index.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hot::ls
{

// The size and the modification time of a file.
struct FileStamp
{
    uint64_t size = 0;
    int64_t mtime = 0;
    bool operator==(const FileStamp& o) const { return size == o.size && mtime == o.mtime; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

// The data of one script. An Entry does not change after set() publishes it. The index stores
// it as std::shared_ptr<const Entry>.
// A later native identity (HOT-35) adds one field here and one map to the index.
struct Entry
{
    std::string path;
    FileStamp stamp; // of the script
    // The stamp of each file in the import closure of the script, in the order of the search.
    std::vector<std::pair<std::string, FileStamp>> dep_stamps;
    std::string digest; // the shard digest of the text
    std::string text;
    std::shared_ptr<const FileRefs> table;
    std::vector<IndexClass> classes;
    std::set<std::string> words; // every identifier word of the text
    // The code words with one or more occurrences that are not an exact resolved ref. This
    // includes unresolved names and the words of raw bodies.
    std::set<std::string> unclaimed_words;
};

class WorkspaceIndex
{
public:
    // Publishes the entry of "path". It replaces the old entry and updates the maps.
    void set(const std::string& path, std::shared_ptr<const Entry> entry);
    void erase(const std::string& path);
    // The entry of "path", or null.
    std::shared_ptr<const Entry> entry(const std::string& path) const;
    // The sorted paths of the files that have the word in the text.
    std::vector<std::string> files_with_word(const std::string& word) const;
    // The sorted paths of the files where the word has an occurrence that is not an exact
    // resolved ref. A later native identity (HOT-35) adds one more map like this one.
    std::vector<std::string> files_with_unclaimed_word(const std::string& word) const;
    // The sorted paths of the files that have a ref with the id (ResolvedRef::id).
    std::vector<std::string> files_with_ref(const std::string& id) const;
    std::vector<std::shared_ptr<const Entry>> all_entries() const;

private:
    using FileMap = std::map<std::string, std::set<std::string>>;
    void add_locked(const std::string& path, const Entry& e);
    void remove_locked(const std::string& path, const Entry& e);
    static std::vector<std::string> lookup(const FileMap& m, const std::string& key);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Entry>> entries_;
    FileMap by_word_, by_unclaimed_, by_ref_;
};

// The stamp of a file. Gives false when the file is missing.
bool stat_file(const std::string& path, FileStamp& out);

// True when the files still have the stamps of the entry. It does a stat of the script and of
// each dependency. It reads no file. A missing file or a changed stamp gives false.
bool fresh(const Entry& e);

// Builds the entry of a script. It is pure: it reads no server state. The shard comes from
// "index" when the digest matches, else from hotc::analyze, and it goes into "index". The
// imports are read from the disk only, with "options.fs" when it is set. The stamps are taken
// at the start, so a change after the call makes fresh() give false.
std::shared_ptr<const Entry> build_entry(const std::string& path, const std::string& text,
    const hotc::TranslateOptions& options, const std::string& digest, SymbolIndex& index,
    hotc::TranslatorSession& session);

} // namespace hot::ls
