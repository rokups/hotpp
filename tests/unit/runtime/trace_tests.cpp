// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the script traceback (hot/runtime/trace.h): the frame stack, the capture at the
// throw point, and the report of an unhandled exception.
#include "hot/runtime.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{

using hot::detail::TraceGuard;

uint32_t depth()
{
    return hot::detail::trace_stack().depth;
}

TEST(Trace, GuardPushesAndPops)
{
    const uint32_t base = depth();
    {
        TraceGuard a("A.Run", "a.hot");
        EXPECT_EQ(depth(), base + 1);
        {
            TraceGuard b("B.Run", "b.hot");
            EXPECT_EQ(depth(), base + 2);
        }
        EXPECT_EQ(depth(), base + 1);
    }
    EXPECT_EQ(depth(), base);
}

TEST(Trace, CaptureListsTheInnermostFrameFirst)
{
    TraceGuard a("Hero.Update", "hero.hot");
    a.line(12);
    TraceGuard b("Gun.Fire", "gun.hot");
    b.line(7);
    EXPECT_EQ(hot::detail::trace_capture(), "  at Gun.Fire (gun.hot:7)\n  at Hero.Update (hero.hot:12)\n");
}

TEST(Trace, LineUpdatesTheTopFrameOnly)
{
    TraceGuard a("A.Run", "a.hot");
    a.line(1);
    {
        TraceGuard b("B.Run", "b.hot");
        b.line(5);
        b.line(6);
    }
    EXPECT_EQ(hot::detail::trace_capture(), "  at A.Run (a.hot:1)\n");
}

TEST(Trace, DeepStackKeepsTheInnermostFrames)
{
    const uint32_t extra = 10;
    std::vector<std::string> names;
    names.reserve(hot::detail::kTraceMax + extra);
    std::vector<TraceGuard*> guards;
    for (uint32_t i = 0; i < hot::detail::kTraceMax + extra; ++i)
    {
        names.push_back("F" + std::to_string(i));
        guards.push_back(new TraceGuard(names.back().c_str(), "f.hot"));
        guards.back()->line(i + 1);
    }
    const std::string text = hot::detail::trace_capture();
    const uint32_t last = hot::detail::kTraceMax + extra - 1;
    EXPECT_EQ(text.rfind("  at F" + std::to_string(last) + " (f.hot:" + std::to_string(last + 1) + ")\n", 0), 0u) << text;
    EXPECT_NE(text.find("  at F" + std::to_string(extra) + " "), std::string::npos) << text;
    EXPECT_EQ(text.find("  at F" + std::to_string(extra - 1) + " "), std::string::npos) << text;
    EXPECT_NE(text.find("  ... " + std::to_string(extra) + " more\n"), std::string::npos) << text;
    while (!guards.empty())
    {
        delete guards.back(); // pops in the reverse order
        guards.pop_back();
    }
}

TEST(Trace, DeepStackRestoresTheOuterFramesAfterTheUnwind)
{
    TraceGuard outer("Outer.Run", "outer.hot");
    outer.line(3);
    TraceGuard next("Next.Run", "next.hot");
    next.line(4);
    const std::string before = hot::detail::trace_capture();
    {
        std::vector<std::string> names;
        names.reserve(70);
        std::vector<TraceGuard*> guards;
        for (uint32_t i = 0; i < 70; ++i)
        {
            names.push_back("Deep" + std::to_string(i));
            guards.push_back(new TraceGuard(names.back().c_str(), "deep.hot"));
            guards.back()->line(i + 1);
        }
        while (!guards.empty())
        {
            delete guards.back();
            guards.pop_back();
        }
    }
    EXPECT_EQ(hot::detail::trace_capture(), before);
    EXPECT_EQ(before, "  at Next.Run (next.hot:4)\n  at Outer.Run (outer.hot:3)\n");
}

#if HOT_TRACEBACK

void inner()
{
    HOT_TRACE_FRAME("Gun.Fire", "gun.hot");
    HOT_TRACE_LINE();
    int* p = nullptr;
    hot::deref(p);
}

void outer()
{
    HOT_TRACE_FRAME("Hero.Update", "hero.hot");
    HOT_TRACE_LINE();
    inner();
}

TEST(Trace, ExceptionKeepsTheStackOfTheThrowPoint)
{
    const uint32_t base = depth();
    try
    {
        outer();
        FAIL() << "no exception";
    }
    catch (const hot::ScriptError& e)
    {
        // The unwind popped the frames, but the exception keeps the text.
        EXPECT_EQ(depth(), base);
        const std::string& t = e.traceback();
        const size_t gun = t.find("  at Gun.Fire (gun.hot:");
        const size_t hero = t.find("  at Hero.Update (hero.hot:");
        ASSERT_NE(gun, std::string::npos) << t;
        ASSERT_NE(hero, std::string::npos) << t;
        EXPECT_LT(gun, hero);
    }
}

TEST(Trace, ReportCarriesTheTraceback)
{
    std::string seen;
    hot::set_script_error_handler([&](const hot::ScriptErrorReport& r) { seen = r.traceback; });
    try
    {
        outer();
    }
    catch (...)
    {
        hot::detail::report_unhandled(nullptr, std::current_exception());
    }
    hot::set_script_error_handler({});
    EXPECT_NE(seen.find("  at Gun.Fire ("), std::string::npos) << seen;
    EXPECT_NE(seen.find("  at Hero.Update ("), std::string::npos) << seen;
}

#else

TEST(Trace, MacrosAreEmptyWhenOff)
{
    HOT_TRACE_FRAME("A.Run", "a.hot");
    HOT_TRACE_LINE();
    const uint32_t before = depth();
    EXPECT_EQ(depth(), before);
}

TEST(Trace, ExceptionHasNoTracebackWhenOff)
{
    TraceGuard a("A.Run", "a.hot");
    int* p = nullptr;
    try
    {
        hot::deref(p);
        FAIL() << "no exception";
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_TRUE(e.traceback().empty());
    }
}

#endif

} // namespace
