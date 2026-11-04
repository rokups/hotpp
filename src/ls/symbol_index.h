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
// The digest covers the text of the file and the texts of the scripts that it imports.
// A write goes to a temporary file first. Then a rename puts it in place.
#pragma once

#include <functional>
#include <optional>
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

struct IndexShard
{
    std::vector<IndexClass> classes;
    IndexClass* find_class(const std::string& name);
};

class SymbolIndex
{
public:
    static constexpr int kFormatVersion = 1;

    // Sets the directory. An empty directory switches the index off.
    void open(const std::string& dir);
    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }

    // The digest of a file content.
    static std::string digest(const std::string& content);
    // The digest of a script and of the scripts that it imports ("using "x.hot";"), also the
    // imports of the imports. A script import is read from the directory of the script, then from
    // the include dirs. When an imported script changes, the digest changes.
    static std::string digest(
        const std::string& script_path, const std::string& content, const std::vector<std::string>& include_dirs);
    // The path of the shard file of a script path.
    std::string shard_path(const std::string& script_path) const;

    // Gives the shard of a script if the version and the digest match. Else gives no value.
    std::optional<IndexShard> load(const std::string& script_path, const std::string& digest);
    // Writes the shard of a script. It replaces the old shard. Gives false on an error.
    bool store(const std::string& script_path, const std::string& digest, const IndexShard& shard);
    // Deletes the shards of missing files, the shards with another version and old temporary
    // files. Gives the number of deleted files.
    int prune();
    // Deletes the shards of the files for which "match" gives true. Gives the number of deleted
    // shards.
    int remove_if(const std::function<bool(const std::string&)>& match);

    int loads() const { return loads_; }
    int stores() const { return stores_; }

private:
    std::string dir_;
    int loads_ = 0, stores_ = 0;
};

} // namespace hot::ls
