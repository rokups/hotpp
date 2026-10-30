// The native-and-std reload scenario. Every way to get a module runs the same checks.
//
// Generation 1 (tests/scripts/native_std_v1.hot) calls the non-inline functions of
// tests/engine/Util.cpp and uses std::vector and std::sort. Generation 2 renames the
// field label to title ([alias]) and adds the field note. The reload must move
// the std::string and the std::vector (the non-trivial move path).
#pragma once

#include "support/reload_assert.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace nativestd
{

using test_support::kDt;
using test_support::Session;

inline void run_native_std_reload(spp::Module v1, spp::Module v2)
{
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(v1), "Tracker"));
    s.commit_at_frame_boundary();
    auto str = [&](const char* n) { return spp::field_ptr<std::string>(s.inst.shell, n); };
    auto vec = [&] { return spp::field_ptr<std::vector<int>>(s.inst.shell, "history"); };

    const std::string label = "tracker label that is longer than the small string buffer";
    ASSERT_NE(str("label"), nullptr);
    EXPECT_EQ(*str("label"), label);

    // Generation 1: the script calls the native functions of Util.cpp.
    s.actor->Position = {3.0f, 4.0f, 0.0f};
    s.world.Tick(kDt);
    s.world.Tick(kDt);
    ASSERT_NE(s.field("dist"), nullptr);
    EXPECT_FLOAT_EQ(*s.field("dist"), 5.0f); // Distance
    EXPECT_FLOAT_EQ(*s.field("lowest"), 1.5f); // std::sort, then Math.Min
    EXPECT_FLOAT_EQ(*s.field("sum"), 7.0f); // Util::Sum
    EXPECT_EQ(s.actor->Name, "(3, 4, 0)"); // FormatVec
    ASSERT_NE(str("described"), nullptr);
    EXPECT_EQ(*str("described"), "(3, 4, 0)@(3, 4, 0)"); // Util::Describe
    ASSERT_NE(vec(), nullptr);
    EXPECT_EQ(*vec(), (std::vector<int>{0, 10}));

    // The reload commits after the next Tick, which is still generation 1.
    std::string error;
    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(v2)));
    s.world.Tick(kDt);
    ASSERT_FALSE(s.commit_failed);
    EXPECT_EQ(spp::active_generation, 2u);

    // The std::string and the std::vector moved to the new block.
    EXPECT_EQ(str("label"), nullptr);
    ASSERT_NE(str("title"), nullptr);
    EXPECT_EQ(*str("title"), label);
    ASSERT_NE(vec(), nullptr);
    EXPECT_EQ(*vec(), (std::vector<int>{0, 10, 20}));
    ASSERT_NE(str("note"), nullptr);
    EXPECT_EQ(*str("note"), "note from generation 2");
    EXPECT_FLOAT_EQ(*s.field("dist"), 5.0f);
    EXPECT_EQ(*str("described"), "(3, 4, 0)@(3, 4, 0)");

    // Generation 2 calls the same native functions and grows the migrated vector.
    s.world.Tick(kDt);
    EXPECT_FLOAT_EQ(*s.field("dist"), std::sqrt(14.0f));
    EXPECT_FLOAT_EQ(*s.field("lowest"), 7.0f);
    EXPECT_FLOAT_EQ(*s.field("sum"), 14.0f);
    EXPECT_EQ(*vec(), (std::vector<int>{0, 10, 20, 30}));
    EXPECT_EQ(*str("title"), label);
}

} // namespace nativestd
