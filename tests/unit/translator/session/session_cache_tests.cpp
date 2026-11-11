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
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // b waits for the entry
    go.set_value();
    a.join();
    b.join();
    EXPECT_TRUE(threw);
    ASSERT_TRUE(b_res.ok) << b_res.diagnostics;
    EXPECT_EQ(session.parse_count(), 2);
}
