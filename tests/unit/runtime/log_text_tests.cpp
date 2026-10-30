// The text of the reload warnings uses readable names (design 6.6): C++ type names and
// class names, not mangled keys and not generated layout names.
#include "spp/runtime.h"

#include <gtest/gtest.h>

#include <string>

namespace
{

TEST(LogText, ReadableTypeDemangles)
{
    EXPECT_EQ(spp::readable_type(spp::type_key<int>()), "int");
    EXPECT_EQ(spp::readable_type(spp::type_key<float>()), "float");
    EXPECT_EQ(spp::readable_type(spp::type_key<std::string>()), "std::string");
    EXPECT_EQ(spp::readable_type(spp::type_key<std::vector<int>>()).find("std::vector<int"), 0u);
    EXPECT_EQ(spp::readable_type("not a mangled name"), "not a mangled name");
    EXPECT_EQ(spp::readable_type(nullptr), "?");
}

TEST(LogText, ReadableLayoutGivesTheClassName)
{
    EXPECT_EQ(spp::readable_layout("HeroState_g2"), "Hero");
    EXPECT_EQ(spp::readable_layout("HeroState_g12"), "Hero");
    EXPECT_EQ(spp::readable_layout("State2"), "State2");
    EXPECT_EQ(spp::readable_layout("State_g2"), "State_g2"); // no class name before the suffix
    EXPECT_EQ(spp::readable_layout("HeroState_gx"), "HeroState_gx");
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
    spp::FieldInfo f1[] = {
        {"title", nullptr, spp::type_key<int>(), 0, sizeof(int), alignof(int), 0, nullptr, nullptr, nullptr}};
    spp::FieldInfo f2[] = {{"title", nullptr, spp::type_key<std::string>(), 0, sizeof(std::string),
        alignof(std::string), 0, nullptr, nullptr, nullptr}};
    const spp::StateLayout l1{"HeroState_g1", sizeof(S1), alignof(S1), 1, f1};
    const spp::StateLayout l2{"HeroState_g2", sizeof(S2), alignof(S2), 1, f2};
    const spp::MigrationPlan plan = spp::build_plan(l1, l2);
    ASSERT_EQ(plan.warnings.size(), 1u);
    const std::string& w = plan.warnings[0];
    EXPECT_NE(w.find("Hero: field title changed its type (int -> std::string)"), std::string::npos) << w;
    EXPECT_EQ(w.find("State_g"), std::string::npos) << w;
    EXPECT_EQ(w.find("__cxx11"), std::string::npos) << w;
}

} // namespace
