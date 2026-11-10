// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// hotc::translator_build_id: the hash of the translator sources (made by the build) and the
// identity of the libclang file (the path, the change time and the size).
#include "hot/hotc_lib.h"

#include "translator_build_id.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef HOT_LIBCLANG_FILE_PATH
#define HOT_LIBCLANG_FILE_PATH ""
#endif

namespace hotc
{

const char* translator_build_id()
{
    static const std::string id = []
    {
        std::string s = HOT_TRANSLATOR_SOURCE_ID;
        const std::filesystem::path lib(HOT_LIBCLANG_FILE_PATH);
        s += "|";
        s += lib.generic_string();
        std::error_code ec1, ec2;
        // A portable build can move the library: then the path does not exist, and only the
        // hash and the path are in the id.
        const auto t = std::filesystem::last_write_time(lib, ec1);
        const auto size = std::filesystem::file_size(lib, ec2);
        if (!ec1 && !ec2)
            s += "|" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count())
                + "|" + std::to_string(size);
        return s;
    }();
    return id.c_str();
}

} // namespace hotc
