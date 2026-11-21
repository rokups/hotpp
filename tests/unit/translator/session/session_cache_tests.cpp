// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests for the cache of hotc::TranslatorSession.
//
// The cache keeps the virtual methods of the native base classes. A hit must give
// the same output as a miss. A change of a header must invalidate the cache.
// CMake defines HOT_SOURCE_DIR.
#include "hot/hotc_lib.h"
#include "hot/vfs.h"
#include "support/stub_translate_test_support.h"
#include "support/translator_test_support.h"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>

namespace
{
void set_pch_env(bool off)
{
#ifdef _WIN32
    _putenv_s("HOT_PCH_INSTANTIATE", off ? "0" : "");
#else
    if (off)
        ::setenv("HOT_PCH_INSTANTIATE", "0", 1);
    else
        ::unsetenv("HOT_PCH_INSTANTIATE");
#endif
}
} // namespace

using namespace translator_test;
using namespace stub_translate_test;

TEST(SessionCache, HitGivesTheSameOutputAsMiss)
{
    hotc::TranslateOptions o = engine_options("player_v1.hot", 1);
    const hotc::TranslateResult alone = hotc::translate(o); // a temporary session
    ASSERT_TRUE(alone.ok) << alone.diagnostics;

    hotc::TranslatorSession session;
    const hotc::TranslateResult miss = hotc::translate(o, session);
    const hotc::TranslateResult hit = hotc::translate(o, session);
    ASSERT_TRUE(miss.ok) << miss.diagnostics;
    ASSERT_TRUE(hit.ok) << hit.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(miss.cpp, alone.cpp);
    EXPECT_EQ(hit.cpp, alone.cpp);
    EXPECT_EQ(hit.slots_out, alone.slots_out);

    // The second generation also hits and agrees with a call without a session.
    hotc::TranslateOptions o2 = engine_options("player_v2.hot", 2);
    o2.slots_in = alone.slots_out;
    const hotc::TranslateResult alone2 = hotc::translate(o2);
    const hotc::TranslateResult hit2 = hotc::translate(o2, session);
    ASSERT_TRUE(hit2.ok) << hit2.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(hit2.cpp, alone2.cpp);
    EXPECT_EQ(hit2.slots_out, alone2.slots_out);
}

TEST(SessionCache, TimingCountsTheParse)
{
    const hotc::TranslateOptions o = engine_options("player_v1.hot", 1);
    hotc::TranslatorSession session;
    const auto sum = [](const hotc::TranslateTiming& t)
    { return t.parse_ns + t.parse_wait_ns + t.front_ns + t.lower_ns + t.check_ns + t.emit_ns; };
    Clock::time_point t = Clock::now();
    const hotc::TranslateResult first = hotc::translate(o, session);
    const int64_t wall1 = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count();
    ASSERT_TRUE(first.ok) << first.diagnostics;
    EXPECT_EQ(first.timing.parses, 1);
    EXPECT_GT(first.timing.parse_ns, 0);
    EXPECT_GT(first.timing.emit_ns, 0);
    EXPECT_LE(sum(first.timing), wall1);

    t = Clock::now();
    const hotc::TranslateResult second = hotc::translate(o, session);
    const int64_t wall2 = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count();
    ASSERT_TRUE(second.ok) << second.diagnostics;
    EXPECT_EQ(second.timing.parses, 0);
    EXPECT_EQ(second.timing.parse_ns, 0);
    EXPECT_LE(sum(second.timing), wall2);
}

TEST(SessionCache, ChangedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslatorSession session;

    EXPECT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    // The new virtual method is not in the header yet. Draw is a plain script function.
    hotc::TranslateResult r = hotc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out;
    EXPECT_EQ(session.parse_count(), 1);

    // The header gets a new virtual method. The new size shows the change.
    write_file(dir.path() / "w.h",
        "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} "
        "virtual void Draw() {} };\n"
        "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    r = hotc::translate(temp_options(dir, widget_script("Draw()", "override ")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.cpp.find("Draw"), std::string::npos);
    EXPECT_EQ(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out; // an override

    // A change with the same size and a new time also invalidates the cache.
    const std::string same_size = read_file((dir.path() / "w.h").string());
    std::string edited = same_size;
    edited.replace(edited.find("Draw"), 4, "Dram");
    write_file(dir.path() / "w.h", edited);
    fs::last_write_time(dir.path() / "w.h", fs::last_write_time(dir.path() / "w.h") + std::chrono::seconds(5));
    r = hotc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out; // not an override
    EXPECT_EQ(session.parse_count(), 3);
    r = hotc::translate(temp_options(dir, widget_script("Dram()", "override ")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(r.slots_out.find("Foo::Dram"), std::string::npos) << r.slots_out; // an override
    EXPECT_EQ(session.parse_count(), 3);
}

TEST(SessionCache, ChangedNestedHeaderInvalidatesTheCache)
{
    TempDir dir;
    write_file(dir.path() / "inner.h", "#define EXTRA\n");
    write_file(dir.path() / "w.h",
        "#include \"inner.h\"\n"
        "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {}\n"
        "#ifdef EXTRA\n virtual void Draw() {}\n#endif\n};\n");
    hotc::TranslatorSession session;
    const hotc::TranslateResult first =
        hotc::translate(temp_options(dir, widget_script("Draw()", "override ")), session);
    EXPECT_TRUE(first.ok) << first.diagnostics;
    EXPECT_EQ(first.slots_out.find("Foo::Draw"), std::string::npos) << first.slots_out; // an override
    write_file(dir.path() / "inner.h", "// no macro now\n");
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, widget_script("Draw()")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_NE(r.slots_out.find("Foo::Draw"), std::string::npos) << r.slots_out; // not an override
    EXPECT_EQ(session.parse_count(), 2);
}

TEST(SessionCache, OtherBaseClassWorksInTheSameSession)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslatorSession session;

    const hotc::TranslateResult a =
        hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    const std::string panel_script = "using \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n";
    const hotc::TranslateResult b = hotc::translate(temp_options(dir, panel_script), session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_NE(b.cpp.find("Layout"), std::string::npos);
    // Both bases are in the cache now. The next calls do not parse.
    const int parses = session.parse_count();
    const hotc::TranslateResult a2 =
        hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session);
    const hotc::TranslateResult b2 = hotc::translate(temp_options(dir, panel_script), session);
    EXPECT_EQ(session.parse_count(), parses);
    EXPECT_EQ(a2.cpp, a.cpp);
    EXPECT_EQ(b2.cpp, b.cpp);
    // A method of one base is a plain script function for the other base.
    const hotc::TranslateResult c = hotc::translate(
        temp_options(dir, "using \"w.h\";\nclass Baz : Panel {\n    void Update(float dt) { }\n}\n"), session);
    EXPECT_TRUE(c.ok) << c.diagnostics;
    EXPECT_NE(c.slots_out.find("Baz::Update"), std::string::npos) << c.slots_out;
}

TEST(SessionCache, UnknownBaseAndBadImportsFail)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    hotc::TranslatorSession session;
    hotc::TranslateResult r =
        hotc::translate(temp_options(dir, "using \"w.h\";\nclass Foo : Nothing {\n    void f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.diagnostics.find("not found"), std::string::npos) << r.diagnostics;
    r = hotc::translate(
        temp_options(dir, "using \"missing.h\";\nclass Foo : Widget {\n    void f() { }\n}\n"), session);
    EXPECT_FALSE(r.ok);
    // A good call still works after the failures.
    EXPECT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
}

TEST(SessionCache, SecondTranslateIsFast)
{
    const hotc::TranslateOptions o = engine_options("player_v1.hot", 1);
    hotc::TranslatorSession session;
    Clock::time_point t = Clock::now();
    ASSERT_TRUE(hotc::translate(o, session).ok);
    const double first = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    t = Clock::now();
    ASSERT_TRUE(hotc::translate(o, session).ok);
    const double second = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    std::cout << "translate first=" << first << " ms, second=" << second << " ms\n";
    RecordProperty("first_ms", static_cast<int>(first));
    RecordProperty("second_ms", static_cast<int>(second));
    EXPECT_LT(second, 150.0); // the target is 50 ms; the bound is wide to avoid a flaky test
}

TEST(SessionCache, SessionKeepsTheHierarchy)
{
    hotc::TranslatorSession session;
    const hotc::TranslateResult a = hier_translate(kIntrusiveStub, "    Item item;\n", false, &session);
    ASSERT_TRUE(a.ok) << a.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    const hotc::TranslateResult b = hier_translate(kIntrusiveStub, "    Item item;\n", false, &session);
    ASSERT_TRUE(b.ok) << b.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_EQ(a.cpp, b.cpp);
    EXPECT_TRUE(has(b.cpp, "    " + kIntr + " item;\n")) << b.cpp;
}

// An edit that keeps the size and the time of the header also invalidates the cache.
TEST(SessionCache, EditWithEqualSizeAndTimeInvalidatesTheCache)
{
    TempDir dir;
    const fs::path header = dir.path() / "w.h";
    write_file(header, kHeaderV1);
    hotc::TranslatorSession session;
    ASSERT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);

    const auto old_time = fs::last_write_time(header);
    std::string edited = read_file(header.string());
    edited.replace(edited.find("Update"), 6, "Updatf");
    write_file(header, edited);
    fs::last_write_time(header, old_time); // the same time and the same size as before

    // Updatf is a new virtual method. Update is a plain script function now.
    const hotc::TranslateResult r = hotc::translate(temp_options(dir, widget_script("Update(float dt)")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out; // not an override
}

// An old header (not recent) gets an edit that resets the time and keeps the size. The ctime or
// the inode shows the edit, and the hash of the content confirms it.
TEST(SessionCache, EditOfAnOldHeaderWithEqualSizeAndTimeInvalidatesTheCache)
{
    for (const bool replace_file : {false, true})
    {
        TempDir dir;
        const fs::path header = dir.path() / "w.h";
        write_file(header, kHeaderV1);
        const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
        fs::last_write_time(header, old_time); // not recent: no hash of the time window
        hotc::TranslatorSession session;
        ASSERT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
        EXPECT_EQ(session.parse_count(), 1);
        // A check without an edit hits.
        ASSERT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
        EXPECT_EQ(session.parse_count(), 1);

        std::string edited = read_file(header.string());
        edited.replace(edited.find("Update"), 6, "Updatf");
        if (replace_file)
        { // a new inode, as an editor or a restore tool makes it
            write_file(dir.path() / "w.tmp", edited);
            fs::rename(dir.path() / "w.tmp", header);
        }
        else
        {
            write_file(header, edited);
        }
        fs::last_write_time(header, old_time);

        const hotc::TranslateResult r = hotc::translate(temp_options(dir, widget_script("Update(float dt)")), session);
        EXPECT_TRUE(r.ok) << r.diagnostics;
        EXPECT_EQ(session.parse_count(), 2) << "replace_file=" << replace_file;
        EXPECT_NE(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;
    }
}

// A touch that keeps the content gives no new parse, also when the ctime changes.
TEST(SessionCache, TouchWithTheSameContentKeepsTheCache)
{
    TempDir dir;
    const fs::path header = dir.path() / "w.h";
    write_file(header, kHeaderV1);
    const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
    fs::last_write_time(header, old_time);
    hotc::TranslatorSession session;
    ASSERT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    write_file(header, kHeaderV1);
    fs::last_write_time(header, old_time);
    ASSERT_TRUE(hotc::translate(temp_options(dir, widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
}

// A new header earlier in the include path hides the header that the cache used.
TEST(SessionCache, NewHeaderEarlierInTheIncludePathInvalidatesTheCache)
{
    TempDir first, second;
    write_file(second.path() / "w.h", kHeaderV1);
    hotc::TranslatorSession session;
    auto options = [&](const std::string& script)
    {
        hotc::TranslateOptions o = temp_options(first, script);
        o.include_dirs = {first.path().string(), second.path().string()};
        return o;
    };
    ASSERT_TRUE(hotc::translate(options(widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);
    EXPECT_TRUE(hotc::translate(options(widget_script("Update(float dt)", "override ")), session).ok);
    EXPECT_EQ(session.parse_count(), 1);

    // The new header has no Update. The method Update is not an override any more.
    write_file(first.path() / "w.h",
        "class Widget { public: virtual ~Widget() {} virtual void Draw() {} };\n"
        "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    const hotc::TranslateResult r = hotc::translate(options(widget_script("Update(float dt)")), session);
    EXPECT_TRUE(r.ok) << r.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
    EXPECT_NE(r.slots_out.find("Foo::Update"), std::string::npos) << r.slots_out;
}

namespace
{

// A file system in memory with hooks. The files have version 0, so the stamp of a file reads it
// again during the parse, under the lock of the entry. The hook runs on the second read of a
// path by one thread. That read is part of the parse.
class HookFs final : public hot::FileSystem
{
public:
    std::function<void(const std::string&)> on_parse_read;
    void set(const std::string& path, const std::string& text) { files_[path] = text; }

    bool read(const std::string& path, std::string& out) override
    {
        const auto it = files_.find(path);
        if (it == files_.end())
            return false;
        bool in_parse = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            in_parse = ++reads_[{std::this_thread::get_id(), path}] >= 2;
        }
        if (in_parse && on_parse_read)
            on_parse_read(path);
        out = it->second;
        return true;
    }
    hot::VfsStat stat(const std::string& path) override
    {
        return files_.count(path) ? hot::VfsStat{true, 0} : hot::VfsStat{};
    }

private:
    std::map<std::string, std::string> files_; // set before the threads start
    std::mutex mutex_;
    std::map<std::pair<std::thread::id, std::string>, int> reads_;
};

hotc::TranslateOptions hook_options(const TempDir& dir, const std::shared_ptr<HookFs>& fs)
{
    const std::string d = dir.path().string();
    fs->set(d + "/w.h", kHeaderV1);
    hotc::TranslateOptions o = temp_options(dir, widget_script("Update(float dt)", "override "));
    o.fs = fs;
    return o;
}

} // namespace

// Two keys parse at the same time. Both parses must reach the hook before either one ends.
TEST(SessionParallel, TwoKeysParseAtTheSameTime)
{
    TempDir dir;
    std::mutex m;
    std::condition_variable cv;
    int arrived = 0;
    bool overlap = true;
    const auto hook = [&](const std::string&)
    {
        std::unique_lock<std::mutex> lock(m);
        if (++arrived == 2)
            cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::seconds(20), [&] { return arrived >= 2; }))
            overlap = false;
    };
    hotc::TranslatorSession session;
    std::vector<std::shared_ptr<HookFs>> fss;
    std::vector<hotc::TranslateOptions> opts;
    for (int i = 0; i < 2; ++i)
    {
        fss.push_back(std::make_shared<HookFs>()); // another file system is another key
        opts.push_back(hook_options(dir, fss.back()));
        fss.back()->on_parse_read = hook;
    }
    std::vector<hotc::TranslateResult> res(2);
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.emplace_back([&, i] { res[i] = hotc::translate(opts[i], session); });
    for (auto& t : threads)
        t.join();
    EXPECT_TRUE(overlap);
    EXPECT_TRUE(res[0].ok) << res[0].diagnostics;
    EXPECT_TRUE(res[1].ok) << res[1].diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
}

// Eight threads, one key: one parse, and equal results.
TEST(SessionParallel, OneKeyParsesOnce)
{
    const hotc::TranslateOptions o = engine_options("player_v1.hot", 1);
    hotc::TranslatorSession session;
    std::vector<hotc::TranslateResult> res(8);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] { res[i] = hotc::translate(o, session); });
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(session.parse_count(), 1);
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_TRUE(res[i].ok) << res[i].diagnostics;
        EXPECT_EQ(res[i].cpp, res[0].cpp);
        EXPECT_EQ(res[i].slots_out, res[0].slots_out);
    }
}

// clear() during a parse: no crash, and the next call parses again.
TEST(SessionParallel, ClearDuringAParse)
{
    TempDir dir;
    auto fs = std::make_shared<HookFs>();
    const hotc::TranslateOptions o = hook_options(dir, fs);
    std::promise<void> inside, go;
    std::shared_future<void> go_f = go.get_future().share();
    std::atomic<bool> once{false};
    fs->on_parse_read = [&](const std::string&)
    {
        if (!once.exchange(true))
        {
            inside.set_value();
            go_f.wait();
        }
    };
    hotc::TranslatorSession session;
    hotc::TranslateResult first;
    std::thread t([&] { first = hotc::translate(o, session); });
    inside.get_future().wait();
    session.clear();
    go.set_value();
    t.join();
    ASSERT_TRUE(first.ok) << first.diagnostics;
    EXPECT_EQ(session.parse_count(), 1);
    const hotc::TranslateResult second = hotc::translate(o, session);
    ASSERT_TRUE(second.ok) << second.diagnostics;
    EXPECT_EQ(session.parse_count(), 2); // the clear dropped the entry
    EXPECT_EQ(second.cpp, first.cpp);
}

// A parse that throws on one thread. The thread that waits on the same key parses again.
TEST(SessionParallel, ThrowingParseLetsTheWaiterParse)
{
    TempDir dir;
    auto fs = std::make_shared<HookFs>();
    const hotc::TranslateOptions o = hook_options(dir, fs);
    std::promise<void> inside, go;
    std::shared_future<void> go_f = go.get_future().share();
    std::atomic<bool> once{false};
    fs->on_parse_read = [&](const std::string&)
    {
        if (!once.exchange(true))
        {
            inside.set_value();
            go_f.wait();
            throw std::runtime_error("test failure in the parse");
        }
    };
    hotc::TranslatorSession session;
    bool threw = false;
    std::thread a(
        [&]
        {
            try
            {
                hotc::translate(o, session);
            }
            catch (const std::exception&)
            {
                threw = true;
            }
        });
    inside.get_future().wait();
    hotc::TranslateResult b_res;
    std::thread b([&] { b_res = hotc::translate(o, session); });
    std::this_thread::sleep_for(std::chrono::milliseconds(900)); // b waits for the entry
    go.set_value();
    a.join();
    b.join();
    EXPECT_TRUE(threw);
    ASSERT_TRUE(b_res.ok) << b_res.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
}

namespace
{

const char* kCppHeaderV1 =
    "#pragma once\n"
    "class Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; } };\n";
const char* kCppHeaderV2 =
    "#pragma once\n"
    "class Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; }\n"
    "    virtual int Extra() { return 2; } };\n";

std::string cpp_script(const std::string& includes, const std::string& body)
{
    return includes + "class Foo : public Widget\n{\npublic:\n" + body + "};\n";
}

} // namespace

namespace
{

const char* kCppHeaderBroken =
    "#pragma once\n"
    "class Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; }\n"
    "    int f() { return ; ) } };\n";

struct CppStep
{
    const char* header; // null: the header stays
    std::string script;
    bool ok; // the plain parse must succeed
};

// A file that changed in the last 50 ms before a parse is not stamped: the unit is not kept. A test
// that counts kept units waits after it writes a file.
void settle()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

hotc::TranslateOptions cpp_options(const TempDir& dir, const std::string& script, const char* name = "t.cpp")
{
    hotc::TranslateOptions o = temp_options(dir, script);
    o.script_name = name;
    return o;
}

// "got" must equal the plain parse (a session with no unit reuse) in all texts.
void expect_same(const hotc::TranslateResult& got, const hotc::TranslateResult& want, const std::string& what)
{
    EXPECT_EQ(got.ok, want.ok) << what << "\n" << got.diagnostics << "\n--\n" << want.diagnostics;
    EXPECT_EQ(got.cpp, want.cpp) << what;
    EXPECT_EQ(got.diagnostics, want.diagnostics) << what;
    EXPECT_EQ(got.slots_out, want.slots_out) << what;
}

// The options of an open document of the language server: the session keeps the probe unit.
hotc::TranslateOptions unit_options(const TempDir& dir, const std::string& script)
{
    hotc::TranslateOptions o = temp_options(dir, script);
    o.probe_unit = 1;
    return o;
}

hotc::TranslateResult plain_translate(const hotc::TranslateOptions& o)
{
    hotc::TranslatorSession plain;
    plain.set_tu_cache_limit(0); // a plain parse with no preamble
    return hotc::translate(o, plain);
}

} // namespace

// A reparse of a kept unit with a new script text must give the same result as a fresh parse.
// The edits change the body, the member list, a header, the include block and add errors in the
// script, in a header and in the include block.
TEST(SessionCache, CppReparseGivesTheSameResultAsAFreshParse)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    const std::string inc = "#include \"w.h\"\n";
    const std::string upd = "    int Update(float dt) override { return 1; }\n";
    const std::vector<CppStep> steps = {
        {nullptr, cpp_script(inc, upd), true},
        {nullptr, cpp_script(inc, "    int Update(float dt) override { return 2; }\n"), true},
        {nullptr, cpp_script(inc, "    int hp = 3;\n    int Update(float dt) override { return hp; }\n"), true},
        {kCppHeaderV2, cpp_script(inc, "    int Extra() override { return 5; }\n"), true},
        {nullptr, cpp_script(inc, "    int Update(float dt) override { return missing; }\n"), false},
        {nullptr, cpp_script(inc + "#include <string>\n", "    int Update(float dt) override { return 4; }\n"), true},
        {kCppHeaderBroken, cpp_script(inc, upd), false}, // a syntax error in an inline body of a header
        {nullptr, cpp_script(inc, upd + "    int Twice() { return 2; }\n"), false}, // the header stays broken
        {kCppHeaderV1, cpp_script(inc, upd), true}, // the header is fixed
        {nullptr, "#include \"nothere.h\"\n" + inc + "class Foo : public Widget {};\n", false},
        {nullptr, cpp_script("#include \"nothere.h\"\n", upd), false},
        {nullptr, cpp_script(inc, upd), true},
        {kCppHeaderV2, cpp_script(inc, upd), true},
        {kCppHeaderV1, cpp_script(inc, upd), true},
    };
    hotc::TranslatorSession shared;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        if (steps[i].header)
            write_file(dir.path() / "w.h", steps[i].header);
        const hotc::TranslateOptions o = cpp_options(dir, steps[i].script);
        const hotc::TranslateResult want = plain_translate(o);
        EXPECT_EQ(want.ok, steps[i].ok) << "step " << i << "\n" << want.diagnostics;
        expect_same(hotc::translate(o, shared), want, "shared, step " + std::to_string(i));
        hotc::TranslatorSession first; // the first parse of a new session builds the preamble
        expect_same(hotc::translate(o, first), want, "first parse, step " + std::to_string(i));
    }
    shared.clear(); // drops the kept units
    EXPECT_EQ(shared.tu_cache_size(), 0u);
    const hotc::TranslateOptions again = cpp_options(dir, steps[0].script);
    EXPECT_TRUE(hotc::translate(again, shared).ok);
}

// A unit that had an error is not kept: the next parse is fresh.
TEST(SessionCache, CppUnitWithAnErrorIsNotKept)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    settle();
    hotc::TranslatorSession s;
    const std::string inc = "#include \"w.h\"\n";
    EXPECT_TRUE(hotc::translate(cpp_options(dir, cpp_script(inc, "")), s).ok);
    EXPECT_EQ(s.tu_cache_size(), 1u);
    EXPECT_FALSE(hotc::translate(cpp_options(dir, cpp_script(inc, "    int x = missing;\n")), s).ok);
    EXPECT_EQ(s.tu_cache_size(), 0u);
    EXPECT_TRUE(hotc::translate(cpp_options(dir, cpp_script(inc, "")), s).ok);
    EXPECT_EQ(s.tu_cache_size(), 1u);
}

// A self-include and a symlinked include give the same result as a plain parse.
TEST(SessionCache, CppSelfIncludeAndSymlinkedIncludeEqualAPlainParse)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    write_file(dir.path() / "self.h",
        "#pragma once\n#include \"self.h\"\n#include \"w.h\"\nstruct SelfBox { int v = 1; };\n");
    std::error_code ec;
    std::filesystem::create_symlink(dir.path() / "w.h", dir.path() / "link.h", ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::string body = "    int Update(float dt) override { return 1; }\n";
    const std::string scripts[] = {
        cpp_script("#include \"self.h\"\n", body),
        cpp_script("#include \"link.h\"\n#include \"w.h\"\n", body),
        cpp_script("#include \"w.h\"\n#include \"link.h\"\n", body + "    int g() { return 3; }\n"),
    };
    hotc::TranslatorSession shared;
    for (int round = 0; round < 2; ++round)
        for (size_t i = 0; i < 3; ++i)
        {
            const hotc::TranslateOptions o = cpp_options(dir, scripts[i]);
            expect_same(hotc::translate(o, shared), plain_translate(o), "script " + std::to_string(i));
        }
}

// The key has the compiler arguments: a changed define drops the unit and the result follows.
TEST(SessionCache, CppChangedDefineGivesAnotherUnit)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "#pragma once\n#ifdef EXTRA\n#define BASE_VALUE 7\n#else\n#define BASE_VALUE 1\n#endif\n"
        "class Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return BASE_VALUE; } };\n");
    settle();
    hotc::TranslatorSession s;
    const std::string script = cpp_script("#include \"w.h\"\n", "    int Update(float dt) override { return BASE_VALUE; }\n");
    hotc::TranslateOptions a = cpp_options(dir, script);
    hotc::TranslateOptions b = a;
    b.defines.push_back("-DEXTRA=1");
    for (int i = 0; i < 2; ++i)
    {
        expect_same(hotc::translate(a, s), plain_translate(a), "no define");
        expect_same(hotc::translate(b, s), plain_translate(b), "with define");
    }
    EXPECT_EQ(s.tu_cache_size(), 2u); // one unit for each argument list
}

// The cache keeps at most the limit; the oldest unit goes; a smaller limit drops units at once.
TEST(SessionCache, CppEvictionAndLimit)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    settle();
    hotc::TranslatorSession s;
    const std::string script = cpp_script("#include \"w.h\"\n", "");
    for (int i = 0; i < 7; ++i)
    {
        const std::string name = "u" + std::to_string(i) + ".cpp";
        EXPECT_TRUE(hotc::translate(cpp_options(dir, script, name.c_str()), s).ok);
        EXPECT_LE(s.tu_cache_size(), 4u);
    }
    EXPECT_EQ(s.tu_cache_size(), 4u);
    s.set_tu_cache_limit(2);
    EXPECT_EQ(s.tu_cache_size(), 2u);
    s.set_tu_cache_limit(0);
    EXPECT_EQ(s.tu_cache_size(), 0u);
    EXPECT_TRUE(hotc::translate(cpp_options(dir, script), s).ok);
    EXPECT_EQ(s.tu_cache_size(), 0u); // the cache is off
}

// A header edit between two translates gives a fresh unit (the stamp check) and the same result.
TEST(SessionCache, CppHeaderEditWithTheSameSizeIsSeenByTheTime)
{
    TempDir dir;
    write_file(dir.path() / "w.h", "#pragma once\nclass Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; } };\n");
    hotc::TranslatorSession s;
    const std::string script = cpp_script("#include \"w.h\"\n", "    int Update(float dt) override { return 1; }\n");
    EXPECT_TRUE(hotc::translate(cpp_options(dir, script), s).ok);
    // The same size; the method name differs in one letter. The nanosecond time changes.
    write_file(dir.path() / "w.h", "#pragma once\nclass Widget { public: virtual ~Widget() {} virtual int Update(float dx) { return 1; } };\n");
    const hotc::TranslateOptions o = cpp_options(dir, script);
    expect_same(hotc::translate(o, s), plain_translate(o), "same size edit");
}

// N threads translate one script with one session: each result equals the plain parse.
TEST(SessionParallel, CppThreadsShareOneSession)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    const std::string inc = "#include \"w.h\"\n";
    hotc::TranslatorSession s;
    const int kThreads = 6, kRounds = 3;
    std::vector<std::string> scripts;
    for (int i = 0; i < kThreads; ++i)
        scripts.push_back(cpp_script(inc, "    int Update(float dt) override { return " + std::to_string(i) + "; }\n"));
    std::vector<hotc::TranslateResult> want;
    for (int i = 0; i < kThreads; ++i)
        want.push_back(plain_translate(cpp_options(dir, scripts[i], ("th" + std::to_string(i) + ".cpp").c_str())));
    std::vector<std::thread> threads;
    std::atomic<int> bad{0};
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r)
            {
                const std::string fname = "th" + std::to_string(t) + ".cpp"; // 6 keys, the limit is 4
                const hotc::TranslateResult got = hotc::translate(cpp_options(dir, scripts[t], fname.c_str()), s);
                if (!got.ok || got.cpp != want[t].cpp || got.slots_out != want[t].slots_out)
                    ++bad;
            }
        });
    for (std::thread& th : threads)
        th.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_LE(s.tu_cache_size(), 4u);
}

// clear() during a parse is safe: the parse ends, its unit is not kept.
TEST(SessionParallel, CppClearDuringTranslateIsSafe)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    hotc::TranslatorSession s;
    const hotc::TranslateOptions o = cpp_options(dir, cpp_script("#include \"w.h\"\n", ""));
    std::atomic<bool> stop{false};
    std::thread clearer([&] {
        while (!stop)
        {
            s.clear();
            std::this_thread::yield();
        }
    });
    const hotc::TranslateResult want = plain_translate(o);
    for (int i = 0; i < 6; ++i)
    {
        const hotc::TranslateResult got = hotc::translate(o, s);
        EXPECT_TRUE(got.ok);
        if (got.ok)
            EXPECT_EQ(got.cpp, want.cpp);
    }
    stop = true;
    clearer.join();
    s.clear();
    EXPECT_EQ(s.tu_cache_size(), 0u);
}

// A header edit with the same size and a reset mtime (touch -r) is seen by the ctime.
TEST(SessionCache, CppEditWithTheSameSizeAndTheOldMtimeIsSeenByTheCtime)
{
    TempDir dir;
    const std::string v1 = "#pragma once\nclass Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; } };\n";
    const std::string v2 = "#pragma once\nclass Widget { public: virtual ~Widget() {} virtual int Updatf(float dt) { return 1; } };\n";
    ASSERT_EQ(v1.size(), v2.size());
    write_file(dir.path() / "w.h", v1);
    const auto old_time = std::filesystem::last_write_time(dir.path() / "w.h");
    settle();
    hotc::TranslatorSession s;
    const std::string script = cpp_script("#include \"w.h\"\n", "    int Update(float dt) override { return 1; }\n");
    EXPECT_TRUE(hotc::translate(cpp_options(dir, script), s).ok);
    EXPECT_EQ(s.tu_cache_size(), 1u);
    write_file(dir.path() / "w.h", v2);
    std::filesystem::last_write_time(dir.path() / "w.h", old_time); // the size and the mtime are as before
    const hotc::TranslateOptions o = cpp_options(dir, script);
    const hotc::TranslateResult want = plain_translate(o);
    EXPECT_FALSE(want.ok); // Update is no override of the new header
    expect_same(hotc::translate(o, s), want, "same size, old mtime");
}

// A new header in an earlier directory of the include search shadows the found one.
TEST(SessionCache, CppShadowingHeaderInAnEarlierIncludeDirectoryIsSeen)
{
    TempDir first, second;
    write_file(second.path() / "w.h", kCppHeaderV1);
    std::filesystem::create_directories(second.path() / "sub");
    write_file(second.path() / "sub" / "y.h", kCppHeaderV1);
    settle();
    hotc::TranslatorSession s;
    auto options = [&](const std::string& script)
    {
        hotc::TranslateOptions o = cpp_options(first, script);
        o.include_dirs = {first.path().string(), second.path().string()};
        return o;
    };
    const std::string body = "    int Update(float dt) override { return 1; }\n";
    const std::string plain = cpp_script("#include \"w.h\"\n", body);
    const std::string sub = cpp_script("#include \"sub/y.h\"\n", body);
    EXPECT_TRUE(hotc::translate(options(plain), s).ok);
    EXPECT_TRUE(hotc::translate(options(sub), s).ok);
    // The new headers have no Update: the method is no override any more.
    write_file(first.path() / "w.h", "#pragma once\nclass Widget { public: virtual ~Widget() {} virtual void Draw() {} };\n");
    std::filesystem::create_directories(first.path() / "sub");
    write_file(first.path() / "sub" / "y.h", "#pragma once\nclass Widget { public: virtual ~Widget() {} virtual void Draw() {} };\n");
    for (const std::string& script : {plain, sub})
    {
        const hotc::TranslateOptions o = options(script);
        const hotc::TranslateResult want = plain_translate(o);
        EXPECT_FALSE(want.ok);
        expect_same(hotc::translate(o, s), want, "shadowed header");
    }
}

// A unit that nobody uses for the idle limit goes at the next call.
TEST(SessionCache, CppIdleUnitIsDisposed)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kCppHeaderV1);
    settle();
    hotc::TranslatorSession s;
    s.set_tu_cache_idle_ms(600); // the translate after the parse must be shorter
    EXPECT_TRUE(hotc::translate(cpp_options(dir, cpp_script("#include \"w.h\"\n", "")), s).ok);
    EXPECT_EQ(s.tu_cache_size(), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    EXPECT_EQ(s.tu_cache_size(), 0u);
}

// Early instantiation in the preamble (-fpch-instantiate-templates): a template that fails to
// instantiate, in a header or in the script, gives the same result and diagnostics as a parse
// without the flag (HOT_PCH_INSTANTIATE=0), and the next parse after a fix works. A bad header
// of one script does not change an other script, because the units have their own keys.
TEST(SessionCache, CppTemplateInstantiationErrorsEqualAParseWithoutEarlyInstances)
{
    const char* tpl =
        "template <class T> struct Chk { static_assert(sizeof(T) == 4, \"need four bytes\"); T v; };\n";
    const std::string good = std::string("#pragma once\n") + tpl +
        "class Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; } };\n"
        "inline int use_good() { Chk<int> c; return c.v; }\n";
    const std::string bad_in_header = std::string("#pragma once\n") + tpl +
        "class Widget { public: virtual ~Widget() {} virtual int Update(float dt) { return 1; } };\n"
        "inline int use_bad() { Chk<char> c; return c.v; }\n"
        "template struct Chk<char>;\n";
    TempDir dir;
    const std::string inc = "#include \"w.h\"\n";
    const std::string upd = "    int Update(float dt) override { return 1; }\n";
    const std::string bad_in_script = "    int Update(float dt) override { Chk<short> s; return s.v; }\n";
    struct Step
    {
        const std::string* header;
        std::string script;
        const char* other_script_name; // a second unit in the same session
    };
    const std::vector<Step> steps = {
        {&good, cpp_script(inc, upd), nullptr},
        {&bad_in_header, cpp_script(inc, upd), nullptr},
        {&bad_in_header, cpp_script(inc, "    int Update(float dt) override { return 2; }\n"), "other.cpp"},
        {&good, cpp_script(inc, upd), "other.cpp"}, // fixed
        {&good, cpp_script(inc, bad_in_script), nullptr},
        {&good, cpp_script(inc, upd), nullptr}, // fixed
    };
    hotc::TranslatorSession shared;
    for (size_t i = 0; i < steps.size(); ++i)
    {
        write_file(dir.path() / "w.h", *steps[i].header);
        settle();
        hotc::TranslateOptions o = cpp_options(dir, steps[i].script);
        hotc::TranslateResult want;
        {
            set_pch_env(true);
            want = plain_translate(o);
            set_pch_env(false);
        }
        EXPECT_EQ(want.ok, i == 0 || i == 3 || i == 5) << "step " << i << "\n" << want.diagnostics;
        if (!want.ok)
            EXPECT_NE(want.diagnostics.find("need four bytes"), std::string::npos) << want.diagnostics;
        expect_same(hotc::translate(o, shared), want, "shared, step " + std::to_string(i));
        if (steps[i].other_script_name)
        {
            const hotc::TranslateOptions other = cpp_options(dir, cpp_script("", "    int v = 1;\n"), steps[i].other_script_name);
            set_pch_env(true);
            const hotc::TranslateResult want_other = plain_translate(other);
            set_pch_env(false);
            EXPECT_EQ(hotc::translate(other, shared).cpp, want_other.cpp) << "other unit, step " << i;
        }
    }
}

// HOT-57: a new native name in the same session reparses the kept probe unit. The preamble (the
// standard prelude and the imports) stays, so the reparse is much faster than the first parse.
// Each step must give the same result as a plain parse (no kept unit).
TEST(SessionCache, ProbeReparseIsFastAndGivesTheSameResultAsAFreshParse)
{
    TempDir dir;
    // "a_inner.h" sorts before "w.h": the list of the dependencies shows a change of the order.
    write_file(dir.path() / "a_inner.h", "#pragma once\n");
    const std::string heavy =
        "#include <regex>\n#include <iostream>\n#include <unordered_map>\n#include \"a_inner.h\"\n";
    write_file(dir.path() / "w.h", heavy + kHeaderV1);
    // A file that changed just before the parse makes the unit not kept: make it older.
    for (const char* f : {"w.h", "a_inner.h"})
        fs::last_write_time(dir.path() / f, fs::last_write_time(dir.path() / f) - std::chrono::seconds(5));
    const std::string panel = "using \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n";
    const std::string panel_draw = "using \"w.h\";\nclass Bar : Panel {\n    override void Draw() { }\n}\n";
    hotc::TranslatorSession session;

    const hotc::TranslateOptions oa = unit_options(dir, widget_script("Update(float dt)", "override "));
    const hotc::TranslateResult a = hotc::translate(oa, session);
    const hotc::TranslateResult a_plain = plain_translate(oa);
    expect_same(a, a_plain, "first");
    EXPECT_EQ(a.dependencies, a_plain.dependencies);
    EXPECT_EQ(a.shadow_paths, a_plain.shadow_paths);
    ASSERT_EQ(a.timing.parses, 1);

    // A new base name: the entry misses it, so the probe parses again. The preamble stays.
    const hotc::TranslateOptions ob = unit_options(dir, panel);
    const hotc::TranslateResult b = hotc::translate(ob, session);
    const hotc::TranslateResult b_plain = plain_translate(ob);
    expect_same(b, b_plain, "new name");
    EXPECT_EQ(b.dependencies, b_plain.dependencies); // the reparse reads the same files
    EXPECT_EQ(b.shadow_paths, b_plain.shadow_paths);
    ASSERT_EQ(b.timing.parses, 1);
    EXPECT_EQ(b.timing.unit_reused, 1); // the preamble stays

    // A header edit gives new facts: Panel gets "Draw".
    write_file(dir.path() / "w.h",
        heavy + "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n"
                "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} virtual void Draw() {} };\n");
    const hotc::TranslateOptions oc = unit_options(dir, panel_draw);
    const hotc::TranslateResult c = hotc::translate(oc, session);
    expect_same(c, plain_translate(oc), "header edit");
    EXPECT_EQ(c.slots_out.find("Bar::Draw"), std::string::npos) << c.slots_out; // an override

    // An import with an error fails, and the fix works again.
    write_file(dir.path() / "w.h", heavy + "class Widget { int x = ; };\n" + kHeaderV1);
    const hotc::TranslateResult d = hotc::translate(ob, session);
    expect_same(d, plain_translate(ob), "broken header");
    EXPECT_FALSE(d.ok);
    write_file(dir.path() / "w.h", heavy + kHeaderV1);
    expect_same(hotc::translate(ob, session), plain_translate(ob), "fixed header");
}

// Two keys with other flags have their own units: a macro of one key is not in the other.
TEST(SessionCache, ProbeUnitsOfOtherFlagsAreNotShared)
{
    TempDir dir;
    write_file(dir.path() / "w.h",
        "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {}\n"
        "#ifdef EXTRA\n virtual void Draw() {}\n#endif\n};\n"
        "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n");
    const std::string panel = "using \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n";
    hotc::TranslatorSession session;
    hotc::TranslateOptions plain = unit_options(dir, panel);
    hotc::TranslateOptions extra = plain;
    extra.defines = {"-DEXTRA"};
    ASSERT_TRUE(hotc::translate(plain, session).ok);
    ASSERT_TRUE(hotc::translate(extra, session).ok);
    // Now a new name for each key: each reparse must keep its own macros.
    plain.script_text = extra.script_text = widget_script("Draw()");
    const hotc::TranslateResult p = hotc::translate(plain, session);
    expect_same(p, plain_translate(plain), "no macro");
    EXPECT_NE(p.slots_out.find("Foo::Draw"), std::string::npos) << p.slots_out; // not an override
    extra.script_text = widget_script("Draw()", "override ");
    const hotc::TranslateResult x = hotc::translate(extra, session);
    expect_same(x, plain_translate(extra), "macro");
    EXPECT_TRUE(x.ok) << x.diagnostics;
}

// HOT-57: a name with the unknown marker (the hole word of a completion probe) needs no parse. No
// header declares it, so the answer is the one of a parse: the result equals the one with no marker.
TEST(SessionCache, MarkedNameNeedsNoParse)
{
    TempDir dir;
    write_file(dir.path() / "w.h",
        std::string("namespace ea { class Scene { public: int hp = 0; }; template <class T> struct function {}; }\n")
            + kHeaderV1);
    const auto script = [](const std::string& body, const std::string& field = "")
    {
        return "using \"w.h\";\nclass Foo : Widget {\n" + field + "    override void Update(float dt) {\n" + body
            + "\n    }\n}\n";
    };
    const std::string fill = script("        ea.Scene s;\n        ea.function<int> f;");
    const std::vector<std::string> probes = {
        script("        ea.__hot_complete x;"),
        script("        ea.function<ea.__hot_complete> g;"),
        script("", "    ea.__hot_complete hot_hole_;\n"),
        script("", "    ea.Scene.__hot_complete hot_hole_;\n"),
    };
    for (const std::string& probe : probes)
    {
        hotc::TranslatorSession on, off;
        hotc::TranslateOptions o = temp_options(dir, fill);
        ASSERT_TRUE(hotc::translate(o, on).ok);
        ASSERT_TRUE(hotc::translate(o, off).ok);
        o.script_text = probe;
        const int n_on = on.parse_count(), n_off = off.parse_count();
        const hotc::TranslateResult want = hotc::translate(o, off);
        EXPECT_GT(off.parse_count(), n_off) << "the probe misses without the marker:\n" << probe;
        o.unknown_marker = "__hot_complete";
        const hotc::TranslateResult got = hotc::translate(o, on);
        EXPECT_EQ(on.parse_count(), n_on) << probe;
        expect_same(got, want, probe);
    }
    // A real new name still parses with the marker on.
    hotc::TranslatorSession s;
    hotc::TranslateOptions o = temp_options(dir, fill);
    o.unknown_marker = "__hot_complete";
    ASSERT_TRUE(hotc::translate(o, s).ok);
    const int n = s.parse_count();
    o.script_text = script("        ea.function<ea.Scene> h;\n        ea.__hot_complete;");
    hotc::translate(o, s);
    EXPECT_GT(s.parse_count(), n);
}

// HOT-57 delta probe: a fresh entry parses only the absent names, facts and queries. Each step
// must give the same result as a new session. A type miss needs no walk of the root.
TEST(SessionCache, DeltaProbeGivesTheSameResultAsANewSession)
{
    TempDir dir;
    const std::string native = "#include <regex>\n#include <iostream>\n#include <unordered_map>\n"
                               "namespace eng {\n"
                               "struct Blob { int v = 0; };\nstruct Extra { int n = 0; };\nstruct Other { int k = 0; };\n"
                               "inline void ReadBlob(const Blob&) {}\ninline void TakeBlob(Blob&) {}\n}\n"
                               "template <class T> class Tmpl { public: virtual ~Tmpl() {} virtual void Step(T v) {} };\n"
                               "class Panel { public: virtual ~Panel() {} virtual void Layout(int w) {} };\n";
    write_file(dir.path() / "w.h",
        native + "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} };\n");
    // A file that changed just before the parse makes the unit not kept: make it older.
    fs::last_write_time(dir.path() / "w.h", fs::last_write_time(dir.path() / "w.h") - std::chrono::seconds(5));
    const std::string head = "using \"w.h\";\nusing eng;\n"
                             "class Foo : Tmpl<int> {\n    override void Step(int v) { }\n"
                             "    void A(Blob b) { ReadBlob(b); }\n}\n";
    const std::string bar = "class Bar : Widget {\n    override void Update(float dt) { }\n";
    hotc::TranslatorSession session;
    const auto step = [&](const std::string& text, const char* what)
    {
        const hotc::TranslateOptions o = unit_options(dir, text);
        const hotc::TranslateResult r = hotc::translate(o, session);
        expect_same(r, plain_translate(o), what);
        EXPECT_TRUE(r.ok) << what << "\n" << r.diagnostics;
        EXPECT_EQ(r.timing.parses, 1) << what;
        return r;
    };
    const hotc::TranslateResult a = step(head + bar + "}\n", "first");
    EXPECT_NE(a.cpp.find("ReadBlob("), std::string::npos) << a.cpp;

    // A new qualified native type: only its facts are absent.
    const hotc::TranslateResult b = step(head + bar + "    eng.Extra e;\n}\n", "type miss");
    EXPECT_EQ(b.timing.unit_reused, 1);
    EXPECT_GT(a.timing.root_walks, 0);
    EXPECT_EQ(b.timing.root_walks, 0); // the trailers and the index answer the type miss

    // A bare type (@decl), a new call (@fn), a new base name and a new template instance base.
    const std::string more = "class Qux : Tmpl<float> {\n    override void Step(float v) { }\n"
                             "    void T(Blob b) { TakeBlob(b); }\n}\n"
                             "class Baz : Panel {\n    override void Layout(int w) { }\n}\n";
    step(head + more + bar + "    eng.Extra e;\n    Other o;\n}\n", "new queries and bases");

    // A header edit between two texts: a full parse with new facts (Widget gets "Draw").
    write_file(dir.path() / "w.h",
        native + "class Widget { public: virtual ~Widget() {} virtual void Update(float dt) {} virtual void Draw() {} };\n");
    const hotc::TranslateResult d =
        step(head + more + bar + "    eng.Extra e;\n    Other o;\n    override void Draw() { }\n}\n", "header edit");
    EXPECT_EQ(d.slots_out.find("Bar::Draw"), std::string::npos) << d.slots_out; // an override
}

// HOT-57: probe_unit 0 (hotc, the background index) keeps no unit and builds no preamble. 1 keeps the unit.
TEST(SessionCache, ProbeUnitModeZeroKeepsNoUnitAndOneKeepsIt)
{
    TempDir dir;
    write_file(dir.path() / "w.h", kHeaderV1);
    fs::last_write_time(dir.path() / "w.h", fs::last_write_time(dir.path() / "w.h") - std::chrono::seconds(5));
    const std::string panel = "using \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n";
    for (const int mode : {0, 1})
    {
        hotc::TranslatorSession session;
        hotc::TranslateOptions a = temp_options(dir, widget_script("Update(float dt)", "override "));
        hotc::TranslateOptions b = temp_options(dir, panel);
        a.probe_unit = b.probe_unit = mode;
        ASSERT_TRUE(hotc::translate(a, session).ok);
        const hotc::TranslateResult r = hotc::translate(b, session); // a new base name: a second parse
        expect_same(r, plain_translate(b), "mode " + std::to_string(mode));
        EXPECT_EQ(r.timing.parses, 1) << mode;
        EXPECT_EQ(r.timing.unit_reused, mode) << mode;
        EXPECT_EQ(session.probe_cache_size(), static_cast<size_t>(mode)) << mode;
    }
}

// HOT-57: two scripts with the same imports share one probe unit. The kept preamble has no "#line" of a
// script, so a reparse for the other script keeps the preamble.
TEST(SessionCache, ScriptsWithTheSameImportsShareOneProbeUnit)
{
    TempDir dir;
    write_file(dir.path() / "w.h", std::string(kHeaderV1) + "class Frame { public: virtual ~Frame() {} virtual void Paint() {} };\n");
    fs::last_write_time(dir.path() / "w.h", fs::last_write_time(dir.path() / "w.h") - std::chrono::seconds(5));
    hotc::TranslatorSession session;
    const auto run = [&](const char* name, const std::string& text)
    {
        hotc::TranslateOptions o = unit_options(dir, text);
        o.script_name = name;
        const hotc::TranslateResult r = hotc::translate(o, session);
        expect_same(r, plain_translate(o), name);
        return r;
    };
    const hotc::TranslateResult a = run("a.hot", widget_script("Update(float dt)", "override "));
    ASSERT_TRUE(a.ok);
    const hotc::TranslateResult b =
        run("b.hot", "\n\nusing \"w.h\";\nclass Bar : Panel {\n    override void Layout(int w) { }\n}\n");
    const hotc::TranslateResult c = run("a.hot", "using \"w.h\";\nclass Baz : Frame {\n    override void Paint() { }\n}\n");
    EXPECT_EQ(b.timing.parses, 1);
    EXPECT_EQ(b.timing.unit_reused, 1);
    EXPECT_EQ(c.timing.parses, 1);
    EXPECT_EQ(c.timing.unit_reused, 1);
    EXPECT_EQ(a.timing.preamble_builds + b.timing.preamble_builds + c.timing.preamble_builds, 1);
    EXPECT_EQ(session.probe_cache_size(), 1u); // one preamble text for both scripts
}
