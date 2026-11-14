// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The name resolver of hot-ls. It reads the table of the resolved names of the translator
// (hotc::ResolvedRef). The server uses it for definition, references and rename.
//
// The module is pure: it does not start clangd and it does not read files.
// The module has no locks. Use it from one thread.
#pragma once

#include "hot/hotc_lib.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hot::ls
{

// The table of one script.
struct FileRefs
{
    std::string file_id; // hotc::ref_file_id of the script
    std::vector<hotc::ResolvedRef> refs; // sorted by offset
    std::vector<hotc::UnresolvedName> unresolved;
    std::vector<std::pair<uint32_t, uint32_t>> raw_ranges; // the begin and the end of each raw body
    std::vector<std::string> imports; // hotc::ref_file_id of each direct script import
    // The syntax errors of the script: the line (from 1) and the message of each error.
    std::vector<std::pair<int, std::string>> errors;
    // Each script class of the file and the name of its base. The base can be a native class.
    std::vector<std::pair<std::string, std::string>> bases;
};

// Sorts the refs and the unresolved names by offset. The translator sorts them already.
// The function is necessary only for a table from another source.
void sort_refs(FileRefs& file);

// The ref that holds the byte "offset" (offset <= at < offset + length). Gives null when no
// ref holds it. The position directly after the name also gives the ref.
const hotc::ResolvedRef* ref_at(const FileRefs& file, uint32_t offset);

// All the refs in the file with the id "id", in the order of the offsets.
std::vector<const hotc::ResolvedRef*> places(const FileRefs& file, const std::string& id);

// The declaration in the file of the symbol with the id "id". Gives null when the
// declaration is not in this file.
const hotc::ResolvedRef* decl_of(const FileRefs& file, const std::string& id);

// True for a symbol that the other files cannot see: a local or a parameter.
bool file_local(hotc::RefKind kind);

// The script imports of all the files. Each file id has the ids of its direct imports.
class ImportGraph
{
public:
    // Sets the direct imports of a file. It replaces the old imports.
    void set(const std::string& file_id, const std::vector<std::string>& imports);
    // Removes the file and its imports.
    void erase(const std::string& file_id);
    // The files that import "file_id" directly or through other files. Each file is in the result
    // one time. "file_id" is not in the result. An import cycle does not cause a loop.
    std::vector<std::string> importers_of(const std::string& file_id) const;
    // The direct imports of "file_id". Empty for a file that the graph does not have.
    std::vector<std::string> imports_of(const std::string& file_id) const;

private:
    std::map<std::string, std::vector<std::string>> imports_;
    std::map<std::string, std::set<std::string>> importers_; // the reverse edges
};

// Adds "edits" (an array of LSP TextEdit) to changes[uri] of a WorkspaceEdit. Then sorts the
// edits of the uri by range. For each range, only the first edit stays: the edits that are in
// changes[uri] come before the new edits. Gives the number of removed edits with a different
// newText (the conflicts). A caller that gets a conflict can refuse the edit.
int merge_edits(nlohmann::json& changes, const std::string& uri, const nlohmann::json& edits);

// One file of a rename: its table and the text that gives the offsets of the table.
struct RenameFile
{
    const FileRefs* table = nullptr;
    std::string_view text;
};

// The conflict check of a rename of "target" (a ref of the symbol) to "new_name". "files" are the
// files that the rename can change: the file of the declaration and its importers. Gives an
// empty string when the new name is free. Else it gives a message with the file and the line.
// A declaration with the new name conflicts when it is in a scope that hides the symbol or that
// the symbol hides:
//   - a member ("Class.member", also an enum value): a member of the same class family. The
//     family is the class, its script bases and its derived classes (FileRefs::bases).
//   - a module symbol: a module declaration in "files".
//   - a local or a parameter: a local or a parameter in the same function, or a name between the
//     declaration and the end of the function. The table has no function ranges: the function
//     is the range between the declarations that are not locals or parameters (conservative).
std::string rename_conflict(
    const std::vector<RenameFile>& files, const hotc::ResolvedRef& target, const std::string& new_name);

} // namespace hot::ls
