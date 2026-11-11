// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Backend::begin_load (the default) and build_jobs().
#include "hot/backend.h"
#include "support/test_env.h"

#include <cstdint>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>

namespace
{

class FakeBackend : public hot::Backend
{
public:
    bool fail = false;
    hot::LoadResult load(const hot::ScriptSource&, uint32_t, hot::LoadMode) override
    {
        hot::LoadResult r;
        r.dependencies = {"a.h"};
        r.iface_hash = 7;
        r.import_hashes = {{"p.hot", 9}};
        if (fail)
        {
            r.error = "bad";
            r.reason = hot::ReloadReason::CompileError;
        }
        return r;
    }
    const char* name() const override { return "fake"; }
};

// T11: a backend whose load throws for one unit.
class ThrowingBackend : public hot::Backend
{
public:
    hot::LoadResult load(const hot::ScriptSource& s, uint32_t, hot::LoadMode) override
    {
        if (s.name == "bad")
            throw std::runtime_error("boom");
        hot::LoadResult r;
        r.iface_hash = 5;
        return r;
    }
    const char* name() const override { return "throwing"; }
};

} // namespace

TEST(BeginLoadMany, DefaultTurnsAnExceptionIntoAFailedEntry)
{
    ThrowingBackend b;
    auto many = b.begin_load_many({{"a", "", {}, ""}, {"bad", "", {}, ""}, {"c", "", {}, ""}}, 1);
    ASSERT_EQ(many.size(), 3u);
    for (auto& p : many)
        ASSERT_NE(p, nullptr);
    EXPECT_TRUE(many[0]->ok);
    EXPECT_FALSE(many[1]->ok);
    EXPECT_EQ(many[1]->head.error, "boom");
    EXPECT_EQ(many[1]->head.reason, hot::ReloadReason::CompileError);
    EXPECT_TRUE(many[2]->ok);
    EXPECT_EQ(many[2]->head.iface_hash, 5u);
}

TEST(BeginLoad, DefaultMatchesLoadOnError)
{
    FakeBackend b;
    b.fail = true;
    auto p = b.begin_load({}, 1);
    EXPECT_FALSE(p->ok);
    EXPECT_EQ(p->head.error, "bad");
    EXPECT_EQ(p->head.reason, hot::ReloadReason::CompileError);
    EXPECT_EQ(p->head.dependencies, std::vector<std::string>{"a.h"});
    EXPECT_EQ(p->head.iface_hash, 7u);
    EXPECT_FALSE(p->head.module.has_value());
    p->compile();
    const hot::LoadResult r = p->finish();
    EXPECT_EQ(r.error, "bad");
    EXPECT_EQ(r.iface_hash, 7u);
    EXPECT_EQ(r.import_hashes.size(), 1u);
}

TEST(BeginLoad, DefaultOkWhenNoError)
{
    FakeBackend b;
    auto p = b.begin_load({}, 1, hot::LoadMode::Fresh);
    EXPECT_TRUE(p->ok);
    EXPECT_TRUE(p->finish().error.empty());
}

TEST(BuildJobs, ReadsEnvironment)
{
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
    EXPECT_EQ(hot::build_jobs(), 1u);
#else
    const unsigned dflt = std::min(hw, hot::kDefaultMaxJobs);
    test_support::set_env("HOT_JOBS", "3");
    EXPECT_EQ(hot::build_jobs(), 3u);
    test_support::set_env("HOT_JOBS", "32");  // an explicit value wins, also above the default limit
    EXPECT_EQ(hot::build_jobs(), 32u);
    test_support::set_env("HOT_JOBS", "4096");
    EXPECT_EQ(hot::build_jobs(), 4096u);
    test_support::set_env("HOT_JOBS", "0");
    EXPECT_EQ(hot::build_jobs(), dflt);
    test_support::set_env("HOT_JOBS", "abc");
    EXPECT_EQ(hot::build_jobs(), dflt);
    test_support::set_env("HOT_JOBS", "5000");
    EXPECT_EQ(hot::build_jobs(), dflt);
    test_support::unset_env("HOT_JOBS");
    EXPECT_EQ(hot::build_jobs(), dflt);
    EXPECT_GE(dflt, 1u);
    EXPECT_LE(dflt, hot::kDefaultMaxJobs);
#endif
}
