// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The text of the reload warnings uses readable names (design 6.6): C++ type names and
// class names, not mangled keys and not generated layout names.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

TEST(LogText, ReadableTypeDemangles)
{
    EXPECT_EQ(hot::readable_type(hot::type_key<int>()), "int");
    EXPECT_EQ(hot::readable_type(hot::type_key<float>()), "float");
    EXPECT_EQ(hot::readable_type(hot::type_key<std::string>()), "std::string");
    EXPECT_EQ(hot::readable_type(hot::type_key<std::vector<int>>()).find("std::vector<int"), 0u);
    EXPECT_EQ(hot::readable_type("not a mangled name"), "not a mangled name");
    EXPECT_EQ(hot::readable_type(nullptr), "?");
}

TEST(LogText, ReadableLayoutGivesTheClassName)
{
    EXPECT_EQ(hot::readable_layout("HeroState_g2"), "Hero");
    EXPECT_EQ(hot::readable_layout("HeroState_g12"), "Hero");
    EXPECT_EQ(hot::readable_layout("State2"), "State2");
    EXPECT_EQ(hot::readable_layout("State_g2"), "State_g2"); // no class name before the suffix
    EXPECT_EQ(hot::readable_layout("HeroState_gx"), "HeroState_gx");
}

struct S1
{
    int title;
};
struct S2
{
    std::string title;
};

TEST(LogText, TypeChangeWarningIsReadable)
{
    hot::FieldInfo f1[] = {
        {"title", nullptr, hot::type_key<int>(), 0, sizeof(int), alignof(int), 0, nullptr, nullptr, nullptr}};
    hot::FieldInfo f2[] = {{"title", nullptr, hot::type_key<std::string>(), 0, sizeof(std::string),
        alignof(std::string), 0, nullptr, nullptr, nullptr}};
    const hot::StateLayout l1{"HeroState_g1", sizeof(S1), alignof(S1), 1, f1};
    const hot::StateLayout l2{"HeroState_g2", sizeof(S2), alignof(S2), 1, f2};
    const hot::MigrationPlan plan = hot::build_plan(l1, l2);
    ASSERT_EQ(plan.warnings.size(), 1u);
    const std::string& w = plan.warnings[0];
    EXPECT_NE(w.find("Hero: field title changed its type (int -> std::string)"), std::string::npos) << w;
    EXPECT_EQ(w.find("State_g"), std::string::npos) << w;
    EXPECT_EQ(w.find("__cxx11"), std::string::npos) << w;
}

} // namespace
