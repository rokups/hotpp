// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// ObjectCache: a disk cache of compiled script objects. A restart of the host finds the object
// of an unchanged script and skips the compiler.
//
// Layout of the cache directory:
//   <dir>/obj/<hex>.o     the object file
//   <dir>/obj/<hex>.d     the list of the headers, one path in each line
//   <dir>/obj/<hex>.json  the manifest: the stamp (path, mtime, size) of each header
// A store writes the manifest last. An entry without a manifest is a miss.
// A process can use the same directory as other processes. No lock is necessary.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hot
{

// A 64-bit FNV-1a hash. The seed is the start value. The same data with two seeds gives two
// hashes that are independent enough for a 128-bit cache key.
class Hash64
{
public:
    static constexpr uint64_t default_seed = 14695981039346656037ULL;

    explicit Hash64(uint64_t seed = default_seed) : h_(seed) {}

    void add_bytes(const void* data, size_t size)
    {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            h_ ^= p[i];
            h_ *= 1099511628211ULL;
        }
    }
    void add_u64(uint64_t value)
    {
        unsigned char b[8];
        for (int i = 0; i < 8; ++i)
            b[i] = static_cast<unsigned char>(value >> (8 * i));
        add_bytes(b, 8);
    }
    // A text with its length in front. So the end of a text is not ambiguous.
    void add_text(const std::string& text)
    {
        add_u64(text.size());
        add_bytes(text.data(), text.size());
    }
    // The final value. It mixes the bits (the finalizer of SplitMix64).
    uint64_t value() const
    {
        uint64_t z = h_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

private:
    uint64_t h_;
};

class ObjectCache
{
public:
    // The key of an entry: 128 bits.
    struct Key
    {
        uint64_t a = 0;
        uint64_t b = 0;
        // 32 lower-case hex digits.
        std::string hex() const;
        bool operator==(const Key& o) const { return a == o.a && b == o.b; }
        bool operator!=(const Key& o) const { return !(*this == o); }
    };

    // All the data that decides the object file.
    struct KeyInputs
    {
        std::string cpp_text;                                           // the translated source
        std::vector<std::string> args;                                  // the normalized compiler arguments
        std::string compiler_id;                                        // the compiler path and version
        std::vector<std::pair<std::string, std::string>> virtual_files; // (path, content) of generated files
    };

    struct Hit
    {
        std::filesystem::path object; // the path of the cached object file
    };

    struct Stats
    {
        uint64_t hits = 0;
        uint64_t misses = 0;
    };

    // "max_bytes" is the size limit of trim(). The value 0 means no limit.
    ObjectCache(std::filesystem::path dir, uint64_t max_bytes);

    // Each field has its length in front. The order of "virtual_files" does not matter.
    static Key make_key(const KeyInputs& inputs);

    // Replaces the "work_dir" prefix of each argument with "@work@". The work directory has the
    // process id, so the key does not depend on it.
    static std::vector<std::string> normalize_args(const std::vector<std::string>& args, const std::string& work_dir);

    // Checks the stamps of the headers. A hit sets the manifest time to now. Any error is a miss.
    // It never throws.
    std::optional<Hit> lookup(const Key& key);

    // Adds an entry. "obj_tmp" is the finished object file: the call moves it into the cache.
    // "depfile_text" is the text of the depfile (Make syntax). Files under "work_dir" have no
    // stamp. On a failure it returns false and sets "error". Two processes can store the same
    // key at the same time.
    bool store(const Key& key, const std::filesystem::path& obj_tmp, const std::string& depfile_text,
        const std::string& work_dir, std::string& error);

    void remove(const Key& key);

    // The path of the object file of an entry. It does not check that the entry exists.
    std::filesystem::path object_path(const Key& key) const;

    // When the entries are larger than the limit, removes the oldest entries (by the time of the
    // manifest) until the size is below 75% of the limit. Removes the old temporary files.
    void trim();

    Stats stats() const;

    // False when the environment variable HOT_CACHE is "0". Else true.
    static bool enabled_by_env();

    // The header list of a depfile in Make syntax. It returns false on a syntax error.
    // Public for the tests.
    static bool parse_depfile(const std::string& text, std::vector<std::string>& deps);

private:
    std::filesystem::path obj_dir() const;
    std::filesystem::path entry_file(const Key& key, const char* ext) const;

    std::filesystem::path dir_;
    uint64_t max_bytes_;
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

} // namespace hot
