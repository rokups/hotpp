// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// TranslateCache: a disk cache of the translate output (hotc::translate). A restart of the host
// finds the output of an unchanged script and skips libclang.
//
// Layout of the cache directory (the same directory as ObjectCache):
//   <dir>/tr/<name>.cpp   the generated C++ text
//   <dir>/tr/<name>.data  the other parts of the result (JSON)
//   <dir>/tr/<name>.json  the manifest: the checks of a lookup and the hashes of the two files
// The key (the hex) has the translator build id and the translate options, but not the slot text
// and not the imported scripts. A key has a maximum of four entries: <name> is "<hex>" or
// "<hex>-<n>". An edit of an imported script thus keeps the entry of the earlier text, and a
// revert of the edit is a hit. The manifest has the rest: the imported scripts (the search and the hash of the text),
// the stamps of the native headers, the files of the host file system, the shadow paths and the
// digest of the slot lines of the classes. A shadow path is a path that a header search tried
// before it found the header. A file at a shadow path is a miss: it hides a header of the parse.
// A lookup does each check again on each entry of the key. A check that cannot run is a miss.
// A store replaces the entry with the same imported texts and slot digest, else a free entry,
// else the entry with the oldest use. A store writes the manifest last. ObjectCache::trim also trims these entries.
#pragma once

#include "hot/hotc_lib.h"
#include "hot/object_cache.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace hot
{

class TranslateCache
{
public:
    struct Stats
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
    };

    // "build_id" empty: hotc::translator_build_id(). A test gives an other id. "extra" is more
    // text for the key, for example the compiler id of a backend (its system headers).
    TranslateCache(std::filesystem::path dir, std::string build_id = std::string(), std::string extra = std::string());

    // The key: the build id, "extra", the current directory and each option that can change the
    // output. The slot text is not in it (the manifest has its digest).
    ObjectCache::Key make_key(const hotc::TranslateOptions& options) const;

    // The result of an earlier translate with the same inputs, or no value. It never throws.
    // The slot parts that belong to other classes come from options.slots_in (hotc::carry_slots).
    std::optional<hotc::TranslateResult> lookup(const hotc::TranslateOptions& options);

    // Adds an entry for a translate with no error. "started" is the time before the translate.
    // A session entry can come from a parse of an earlier translate
    // (TranslateResult::parse_start_ns). The earlier of the two times is the parse start. A header
    // with a change time after the parse start minus 2 seconds can have changed during the
    // parse, so there is no entry. It gives false and sets "error" when it writes no entry.
    bool store(const hotc::TranslateOptions& options, const hotc::TranslateResult& result,
        std::filesystem::file_time_type started, std::string& error);

    void remove(const ObjectCache::Key& key);

    // Sets "extra" (see the constructor). Call it before the first lookup or store.
    void set_extra(std::string extra) { extra_ = std::move(extra); }

    Stats stats() const { return {hits_.load(), misses_.load()}; }

    // False when HOT_TRANSLATE_CACHE or HOT_CACHE is "0". Else true.
    static bool enabled_by_env();

private:
    std::filesystem::path entry_file(const ObjectCache::Key& key, int entry, const char* ext) const;
    std::optional<hotc::TranslateResult> lookup_entry(
        const hotc::TranslateOptions& options, const ObjectCache::Key& key, int entry);

    std::filesystem::path dir_;
    std::string build_id_;
    std::string extra_;
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

} // namespace hot
