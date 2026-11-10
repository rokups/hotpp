// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The file and JSON helpers of the disk caches (ObjectCache and TranslateCache). Internal.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace hot::cache_io
{

// A JSON value. The manifest needs only text, integers, arrays and objects.
struct Json
{
    enum class Type
    {
        Null,
        Number,
        String,
        Array,
        Object
    };
    Type type = Type::Null;
    int64_t number = 0;
    std::string text;
    std::vector<Json> items;                         // an array
    std::vector<std::pair<std::string, Json>> members; // an object

    const Json* find(const std::string& name) const
    {
        for (const auto& m : members)
            if (m.first == name)
                return &m.second;
        return nullptr;
    }
};

// The time of a file in nanoseconds.
int64_t to_ns(std::filesystem::file_time_type t);
// The path of a temporary file in the directory of "final_path".
std::filesystem::path temp_name(const std::filesystem::path& final_path);
// Writes the file with a temporary file and a rename.
bool write_file_atomic(const std::filesystem::path& final_path, const std::string& content, std::string& error);
bool read_file(const std::filesystem::path& path, std::string& content);
// Gives false on a syntax error or on text after the value.
bool parse_json(const std::string& text, Json& out);
// Appends the JSON form of the text, with the quotes.
void append_json_string(std::string& out, const std::string& s);

} // namespace hot::cache_io
