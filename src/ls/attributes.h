// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The attribute table of hot-ls (M5, step 19).
//
// The table lists the attribute names that the parser knows. Each row has a short doc text and
// the argument forms. A test compares the names with the parser, so the table cannot drift.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hot::ls
{

struct AttributeInfo
{
    std::string name;
    std::string doc;
    // The count of string arguments. An argument is free-form text.
    int arity = 0;
    // The names of the named arguments. The parser has no named argument now, so each row is empty.
    std::vector<std::string> named;
    // The place where the attribute is valid, for the detail text.
    std::string where;
};

// All attributes of the parser, in a fixed order.
const std::vector<AttributeInfo>& attribute_table();

// The row of a name, or null.
const AttributeInfo* find_attribute(const std::string& name);

// The completion list at the offset. "arg" is false after "[" (the names) and true inside the
// parentheses of an attribute (the named arguments). The list is an LSP CompletionList.
// The names of the attribute classes that the text declares ("[attribute] class Name;") are in it.
nlohmann::json attribute_completion(std::string_view text, std::size_t offset, std::size_t prefix_start, bool arg);

} // namespace hot::ls
