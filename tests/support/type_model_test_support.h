// Helpers for the tests of the type model: a stub builder and a fixture with a type model.
#pragma once

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

#include "types.h"

namespace type_model_test {

using namespace spc;

inline ast::Stub stub(const std::string& name, const std::string& handle = "", bool is_value = false)
{
    ast::Stub s;
    s.name = name;
    s.is_native = true;
    s.is_value = is_value;
    s.handle = handle;
    return s;
}

struct Fixture {
    TypeModel model;
    Diagnostics diags;
    LineMap map{""};
    void add(const std::vector<ast::Stub>& stubs) { model.add_stubs(stubs, diags, map); }
};

inline const char* kShared = "std::shared_ptr<$T>";

// Builds a base callback from a table of class name to direct bases.
inline TypeModel::BaseFn bases_of(std::map<std::string, std::vector<std::string>> table)
{
    return [table](const std::string& name) {
        auto it = table.find(name);
        return it == table.end() ? std::vector<std::string>{} : it->second;
    };
}

}  // namespace type_model_test
