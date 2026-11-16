// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the core of the workspace index (HOT-34). They need no clangd and no thread.
#include "script_text.h"
#include "workspace_index.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <thread>

namespace fs = std::filesystem;
using namespace hot::ls;

namespace
{
std::shared_ptr<Entry> make_entry(const std::string& path, std::set<std::string> words,
    std::set<std::string> unclaimed, const std::vector<std::string>& ref_ids)
{
    auto e = std::make_shared<Entry>();
    e->path = path;
    e->words = std::move(words);
    e->unclaimed_words = std::move(unclaimed);
    auto t = std::make_shared<FileRefs>();
    for (const std::string& id : ref_ids)
    {
        hotc::ResolvedRef r;
        r.id = id;
        t->refs.push_back(r);
    }
    e->table = t;
    return e;
}

FileStamp stamp_now(const std::string& path)
{
    FileStamp s;
    stat_file(path, s);
    return s;
}

struct TempDir
{
    fs::path dir;
    TempDir()
    {
        dir = fs::temp_directory_path() / ("hot_wsi_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string write(const std::string& name, const std::string& text, int age_s = 0) const
    {
        const fs::path p = dir / name;
        {
            std::ofstream(p, std::ios::binary) << text;
        }
        if (age_s)
            fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(age_s));
        return p.string();
    }
};

struct Built
{
    std::string path;
    std::shared_ptr<const Entry> entry;
};

hotc::TranslateOptions options_for(const std::string& path)
{
    hotc::TranslateOptions o;
    o.script_name = path;
    o.generation = 1;
    o.resolve_refs = true;
    return o;
}

// build_entry() with the deps of one cached_digest call, as the worker does it.
std::shared_ptr<const Entry> build_here(const std::string& path, const FileStamp& stamp, const std::string& text,
    const hotc::TranslateOptions& options, const std::string& digest, SymbolIndex& index,
    hotc::TranslatorSession& session, bool* analyzed = nullptr)
{
    std::vector<SymbolIndex::DepStamp> deps;
    index.cached_digest(path, text, options.include_dirs, &deps);
    return build_entry(path, stamp, text, options, digest, deps, index, session, analyzed);
}
} // namespace

TEST(WorkspaceIndex, SetReplaceEraseUpdateTheMaps)
{
    WorkspaceIndex idx;
    idx.set("a", make_entry("a", {"x", "y"}, {"y"}, {"f#1", "f#2"}));
    idx.set("b", make_entry("b", {"y", "z"}, {}, {"f#2"}));
    EXPECT_EQ(idx.files_with_word("y"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(idx.files_with_word("x"), (std::vector<std::string>{"a"}));
    EXPECT_EQ(idx.files_with_unclaimed_word("y"), (std::vector<std::string>{"a"}));
    EXPECT_EQ(idx.files_with_ref("f#2"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(idx.all_entries().size(), 2u);

    // A replace drops the old words and refs of the file.
    idx.set("a", make_entry("a", {"q"}, {"q"}, {"f#3"}));
    EXPECT_EQ(idx.files_with_word("y"), (std::vector<std::string>{"b"}));
    EXPECT_TRUE(idx.files_with_word("x").empty());
    EXPECT_EQ(idx.files_with_word("q"), (std::vector<std::string>{"a"}));
    EXPECT_TRUE(idx.files_with_unclaimed_word("y").empty());
    EXPECT_EQ(idx.files_with_unclaimed_word("q"), (std::vector<std::string>{"a"}));
    EXPECT_EQ(idx.files_with_ref("f#2"), (std::vector<std::string>{"b"}));
    EXPECT_TRUE(idx.files_with_ref("f#1").empty());
    EXPECT_EQ(idx.all_entries().size(), 2u);

    idx.erase("b");
    EXPECT_EQ(idx.entry("b"), nullptr);
    EXPECT_NE(idx.entry("a"), nullptr);
    EXPECT_TRUE(idx.files_with_word("z").empty());
    EXPECT_TRUE(idx.files_with_ref("f#2").empty());
    idx.erase("missing"); // no effect
    EXPECT_EQ(idx.all_entries().size(), 1u);
}

TEST(WorkspaceIndex, DepMapFollowsSetReplaceErase)
{
    const auto with_deps = [](const std::string& path, const std::vector<std::string>& deps)
    {
        auto e = std::make_shared<Entry>(*make_entry(path, {}, {}, {}));
        for (const std::string& d : deps)
            e->dep_stamps.emplace_back(d, FileStamp{1, 2});
        return std::shared_ptr<const Entry>(e);
    };
    WorkspaceIndex idx;
    idx.set("a", with_deps("a", {"d1", "d2"}));
    idx.set("b", with_deps("b", {"d2", "missing.h"}));
    EXPECT_EQ(idx.files_with_dep("d1"), (std::vector<std::string>{"a"}));
    EXPECT_EQ(idx.files_with_dep("d2"), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(idx.files_with_dep("missing.h"), (std::vector<std::string>{"b"}));

    // A replace drops the old deps of the file.
    idx.set("a", with_deps("a", {"d3"}));
    EXPECT_TRUE(idx.files_with_dep("d1").empty());
    EXPECT_EQ(idx.files_with_dep("d2"), (std::vector<std::string>{"b"}));
    EXPECT_EQ(idx.files_with_dep("d3"), (std::vector<std::string>{"a"}));

    idx.erase("b");
    EXPECT_TRUE(idx.files_with_dep("d2").empty());
    EXPECT_TRUE(idx.files_with_dep("missing.h").empty());
    idx.erase("a");
    EXPECT_TRUE(idx.files_with_dep("d3").empty());
}

TEST(WorkspaceIndex, BuiltEntriesFeedTheMaps)
{
    TempDir tmp;
    const std::string path = tmp.write("a.hot", "class A\n{\n    int count = 0;\n    void bump() { count = count + 1; }\n}\n");
    SymbolIndex index; // switched off: no directory
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), "class A\n{\n    int count = 0;\n    void bump() { count = count + 1; }\n}\n",
        options_for(path), "d1", index, session);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->classes.size(), 1u);
    EXPECT_TRUE(e->words.count("bump"));
    EXPECT_FALSE(e->table->refs.empty());
    WorkspaceIndex idx;
    idx.set(path, e);
    EXPECT_EQ(idx.files_with_word("count"), (std::vector<std::string>{path}));
    EXPECT_EQ(idx.files_with_ref(e->table->refs.front().id), (std::vector<std::string>{path}));
    EXPECT_TRUE(fresh(*e));
}

TEST(WorkspaceIndex, UnclaimedWordsAgreeWithTableLacksWord)
{
    const std::vector<std::string> scripts = {
        // a plain class: every member name is a resolved ref
        "class A\n{\n    int count = 0;\n    void bump() { count = count + 1; }\n}\n",
        // a native type, words in a comment and in a string
        "// bump the counter\nclass B\n{\n    std::string name = \"count\";\n    int size() { return (int)name.size(); }\n}\n",
        // an unresolved name
        "class C\n{\n    int f() { return missing_name + 1; }\n    int missing_name2 = 0;\n}\n",
    };
    TempDir tmp;
    SymbolIndex index;
    hotc::TranslatorSession session;
    int n = 0;
    for (const std::string& text : scripts)
    {
        const std::string path = tmp.write("s" + std::to_string(n++) + ".hot", text);
        const auto e = build_here(path, stamp_now(path), text, options_for(path), "d", index, session);
        ASSERT_TRUE(e);
        ASSERT_FALSE(e->words.empty());
        for (const std::string& w : e->words)
            EXPECT_EQ(table_lacks_word_in(*e->table, text, w), e->unclaimed_words.count(w) == 0) << path << " word " << w;
        // every unclaimed word is a word of the text
        for (const std::string& w : e->unclaimed_words)
            EXPECT_TRUE(e->words.count(w)) << w;
        EXPECT_TRUE(table_lacks_word_in(*e->table, text, "not_in_the_text"));
        if (n == 3)
            EXPECT_TRUE(e->unclaimed_words.count("missing_name"));
    }
}

TEST(WorkspaceIndex, FreshFollowsTheStampsOfTheScriptAndOfItsImports)
{
    TempDir tmp;
    const std::string lib = tmp.write("lib.hot", "int helper() { return 1; }\n", -100);
    const std::string text = "using \"lib.hot\";\nclass M\n{\n    int f() { return helper(); }\n}\n";
    const std::string path = tmp.write("main.hot", text, -100);
    SymbolIndex index;
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), text, options_for(path), "d", index, session);
    ASSERT_TRUE(e);
    ASSERT_EQ(e->dep_stamps.size(), 1u);
    EXPECT_TRUE(fresh(*e));
    EXPECT_TRUE(fresh(*e)); // no change of state

    tmp.write("lib.hot", "int helper() { return 2; }\n", -50); // same size, new mtime
    EXPECT_FALSE(fresh(*e));

    const auto e2 = build_here(path, stamp_now(path), text, options_for(path), "d", index, session);
    EXPECT_TRUE(fresh(*e2));
    tmp.write("main.hot", text + "// more\n", -40);
    EXPECT_FALSE(fresh(*e2));

    const auto e3 = build_here(path, stamp_now(path), text + "// more\n", options_for(path), "d", index, session);
    EXPECT_TRUE(fresh(*e3));
    fs::remove(lib);
    EXPECT_FALSE(fresh(*e3));
}

TEST(WorkspaceIndex, BuildEntryKeepsTheStampTakenBeforeTheRead)
{
    TempDir tmp;
    const std::string text = "class A\n{\n    int f() { return 1; }\n}\n";
    const std::string path = tmp.write("a.hot", text, -100);
    SymbolIndex index;
    hotc::TranslatorSession session;
    FileStamp old = stamp_now(path);
    old.mtime -= 1000; // a stamp from before a later write
    const auto e = build_here(path, old, text, options_for(path), "d", index, session);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->stamp, old);
    EXPECT_FALSE(fresh(*e));
}

TEST(WorkspaceIndex, FreshSeesAnImportThatAppearsEarlierInTheSearch)
{
    TempDir tmp;
    fs::create_directories(tmp.dir / "inc");
    const std::string lib = tmp.write("inc/lib.hot", "int helper() { return 1; }\n", -100);
    const std::string text = "using \"lib.hot\";\nclass M\n{\n    int f() { return helper(); }\n}\n";
    const std::string path = tmp.write("main.hot", text, -100);
    hotc::TranslateOptions options = options_for(path);
    options.include_dirs = {(tmp.dir / "inc").string()};
    SymbolIndex index;
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), text, options, "d", index, session);
    ASSERT_TRUE(e);
    EXPECT_TRUE(fresh(*e));
    std::map<std::string, FileStamp> memo;
    EXPECT_TRUE(fresh(*e, &memo)); // a missing candidate stays equal
    EXPECT_FALSE(memo.empty());
    tmp.write("lib.hot", "int helper() { return 2; }\n", -100); // the script directory comes first
    EXPECT_FALSE(fresh(*e));
}

// HOT-34: an index with no script ends its progress. The ready state needs finish_scheduling().
TEST(WorkspaceIndex, AnEmptyWorkspaceIsReadyAfterTheSchedulingEnds)
{
    WorkspaceIndex index;
    EXPECT_FALSE(index.stats().ready);
    index.finish_scheduling();
    const IndexStats st = index.stats();
    EXPECT_TRUE(st.ready);
    EXPECT_EQ(st.total, 0);
    EXPECT_GE(st.ready_ms, 0);
}

// HOT-34: with queued scripts and no worker, the index is not ready and has no ready time.
TEST(WorkspaceIndex, QueuedScriptsKeepTheIndexNotReadyUntilTheyFinish)
{
    WorkspaceIndex index;
    index.schedule("a.hot", JobConfig{}, IndexLevel::Rest);
    index.schedule("b.hot", JobConfig{}, IndexLevel::Rest);
    index.finish_scheduling();
    const IndexStats st = index.stats();
    EXPECT_FALSE(st.ready);
    EXPECT_EQ(st.total, 2);
    EXPECT_EQ(st.ready_ms, -1);
}

namespace
{
std::shared_ptr<const Entry> entry_with_deps(const std::string& path, const std::vector<std::string>& deps)
{
    auto e = std::make_shared<Entry>(*make_entry(path, {"w"}, {}, {"f#1"}));
    for (const std::string& d : deps)
        e->dep_stamps.emplace_back(d, FileStamp{1, 2});
    return e;
}
} // namespace

// HOT-34: invalidate queues the path and the scripts that import it. The old entry stays.
TEST(WorkspaceIndex, InvalidateQueuesThePathAndItsImporters)
{
    WorkspaceIndex idx;
    // The three scripts do not exist: each job fails. Then no job is queued and no worker runs.
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    for (const char* p : {"lib.hot", "main.hot", "other.hot"})
        idx.schedule(p, JobConfig{}, IndexLevel::Rest);
    idx.start(symbols, session, 1, nullptr);
    for (int i = 0; i < 6000 && idx.stats().pending > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    idx.stop();
    ASSERT_EQ(idx.stats().pending, 0);
    idx.set("lib.hot", entry_with_deps("lib.hot", {}));
    idx.set("main.hot", entry_with_deps("main.hot", {"lib.hot"}));
    idx.set("other.hot", entry_with_deps("other.hot", {"x.h"}));
    EXPECT_EQ(idx.stats().pending, 0);
    const auto old = idx.entry("lib.hot");
    idx.invalidate("lib.hot");
    EXPECT_EQ(idx.stats().pending, 2); // lib.hot and main.hot
    EXPECT_EQ(idx.entry("lib.hot"), old);
    EXPECT_NE(idx.entry("main.hot"), nullptr);
    idx.invalidate("lib.hot");
    EXPECT_EQ(idx.stats().pending, 2); // a queued job is not queued twice
    idx.invalidate("unknown.hot"); // no effect
    EXPECT_EQ(idx.stats().pending, 2);
}

TEST(WorkspaceIndex, ForgetRemovesTheEntryTheSlotAndTheJob)
{
    WorkspaceIndex idx;
    idx.schedule("a.hot", JobConfig{}, IndexLevel::Rest);
    idx.schedule("b.hot", JobConfig{}, IndexLevel::Rest);
    idx.set("a.hot", entry_with_deps("a.hot", {"d.h"}));
    EXPECT_EQ(idx.stats().pending, 2);
    idx.forget("a.hot");
    EXPECT_EQ(idx.entry("a.hot"), nullptr);
    EXPECT_TRUE(idx.files_with_word("w").empty());
    EXPECT_TRUE(idx.files_with_ref("f#1").empty());
    EXPECT_TRUE(idx.files_with_dep("d.h").empty());
    EXPECT_EQ(idx.stats().pending, 1);
    EXPECT_EQ(idx.stats().total, 1);
    idx.invalidate("a.hot"); // the slot is gone: no job
    EXPECT_EQ(idx.stats().pending, 1);
    idx.forget("a.hot"); // no effect
}

namespace
{
const char* const kTextOne = "class M\n{\n    int f() { return 1; }\n}\n";
const char* const kTextTwo = "class M\n{\n    int g() { return 2; }\n}\n";

JobConfig config_for(const std::string& path)
{
    JobConfig c;
    c.options = options_for(path);
    return c;
}

const auto kLong = std::chrono::seconds(30);
} // namespace

// HOT-34: a change on the disk with no invalidate call gives a new entry from wait_for.
TEST(WorkspaceIndex, WaitForReplacesAnEntryAfterADiskChangeWithoutAnEvent)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto first = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    ASSERT_TRUE(first.missing.empty());
    const auto old = first.entries.at(path);
    EXPECT_NE(old->text.find("int f()"), std::string::npos);

    tmp.write("main.hot", kTextTwo, -50); // no invalidate call
    const auto second = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(second.missing.empty());
    const auto fresh_entry = second.entries.at(path);
    EXPECT_NE(fresh_entry, old);
    EXPECT_NE(fresh_entry->text.find("int g()"), std::string::npos);
    EXPECT_EQ(idx.entry(path), fresh_entry);
}

// HOT-34: a change of an import gives a new entry of the importer.
TEST(WorkspaceIndex, WaitForReplacesTheEntryOfAnImporterAfterAnImportChange)
{
    TempDir tmp;
    const std::string lib = tmp.write("lib.hot", "int helper() { return 1; }\n", -100);
    const std::string path = tmp.write("main.hot", "using \"lib.hot\";\nclass M\n{\n    int f() { return helper(); }\n}\n", -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    idx.schedule(lib, config_for(lib), IndexLevel::Rest);
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto first = idx.wait_for({lib, path}, nullptr, std::chrono::steady_clock::now() + kLong);
    ASSERT_TRUE(first.missing.empty());
    const auto old = first.entries.at(path);
    ASSERT_EQ(old->dep_stamps.size(), 1u);

    tmp.write("lib.hot", "int helper() { return 2; }\n", -50);
    const auto second = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(second.missing.empty());
    const auto now_entry = second.entries.at(path);
    EXPECT_NE(now_entry, old);
    EXPECT_EQ(now_entry->dep_stamps.front().second, stamp_now(lib));
}

// HOT-34: a deadline of now checks the stamps, queues the stale path and gives it as missing.
TEST(WorkspaceIndex, WaitForWithDeadlineNowGivesTheStalePathAsMissing)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), kTextOne, options_for(path), "d", symbols, session);
    WorkspaceIndex idx; // no worker: the queued job stays
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.set(path, e);
    idx.finish_scheduling();
    const auto ok = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now());
    EXPECT_TRUE(ok.missing.empty());
    EXPECT_EQ(ok.entries.at(path), e);

    tmp.write("main.hot", kTextTwo, -50);
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now());
    EXPECT_EQ(snap.missing, (std::vector<std::string>{path}));
    EXPECT_TRUE(snap.entries.empty());
    EXPECT_EQ(idx.entry(path), e);
    EXPECT_EQ(idx.stats().pending, 1); // the stale path is queued
}

// HOT-34: a path that failed, or that the index does not know, is missing.
TEST(WorkspaceIndex, WaitForGivesAFailedPathAsMissing)
{
    TempDir tmp;
    const std::string path = (tmp.dir / "none.hot").string(); // no such file
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto snap = idx.wait_for({path, "unknown.hot"}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    EXPECT_EQ(snap.missing, (std::vector<std::string>{path, "unknown.hot"}));
    EXPECT_TRUE(snap.entries.empty());
}

// HOT-34: an entry that a worker publishes in the call is valid by generation and by a stamp check
// with a new memo. The test publishes the entry of the new text from poll(). The call must end
// after one poll. The first memo holds the stamp of the old round, so it must not decide.
TEST(WorkspaceIndex, WaitForDoesNotLoopOnAnEntryPublishedDuringTheCall)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    const auto old = build_here(path, stamp_now(path), kTextOne, options_for(path), "d", symbols, session);
    WorkspaceIndex idx; // no worker
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.set(path, old);
    tmp.write("main.hot", kTextTwo, -50); // the old entry is stale
    auto published = std::make_shared<Entry>(*old);
    published->text = kTextTwo;
    published->stamp = stamp_now(path);
    int polls = 0;
    const auto snap = idx.wait_for({path},
        [&]
        {
            if (++polls == 1)
                idx.set(path, published); // the generation is the current one
        },
        std::chrono::steady_clock::now() + kLong);
    EXPECT_TRUE(snap.missing.empty());
    EXPECT_EQ(snap.entries.at(path), published);
    EXPECT_EQ(polls, 1);
}

// HOT-34: a slot that set() made has no config. invalidate and wait_for queue no job for it.
TEST(WorkspaceIndex, ASlotWithNoConfigGetsNoJobAndItsStalePathIsMissing)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), kTextOne, options_for(path), "d", symbols, session);
    WorkspaceIndex idx;
    idx.set(path, e); // no schedule(): no config
    idx.invalidate(path);
    EXPECT_EQ(idx.stats().pending, 0);
    EXPECT_EQ(idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong).entries.at(path), e);

    tmp.write("main.hot", kTextTwo, -50);
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    EXPECT_EQ(snap.missing, (std::vector<std::string>{path}));
    EXPECT_TRUE(snap.entries.empty());
    EXPECT_EQ(idx.stats().pending, 0);
    EXPECT_EQ(idx.entry(path), e);
}

namespace
{
// A hook that blocks the first job of a path until the test releases it.
struct JobGate
{
    std::promise<void> entered, release;
    std::shared_future<void> release_future = release.get_future().share();
    std::atomic<int> calls{0};
    std::function<void(const std::string&)> hook()
    {
        return [this](const std::string&)
        {
            if (calls++ == 0)
            {
                entered.set_value();
                release_future.wait();
            }
        };
    }
};
} // namespace

// HOT-34: an invalidate call in the middle of a job drops the result and runs the job again.
TEST(WorkspaceIndex, InvalidateDuringAJobDropsTheResultAndRunsAgain)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    JobGate gate;
    idx.set_job_hook(gate.hook());
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    gate.entered.get_future().wait(); // the job has read kTextOne
    tmp.write("main.hot", kTextTwo, -50);
    idx.invalidate(path);
    gate.release.set_value();
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(snap.missing.empty());
    EXPECT_NE(snap.entries.at(path)->text.find("int g()"), std::string::npos);
    EXPECT_GE(idx.stats().analyzed, 2);
    EXPECT_EQ(idx.all_entries().size(), 1u);
}

// HOT-34: ensure() on a path with a running job does not count a change. The job result stays.
TEST(WorkspaceIndex, EnsureKeepsARunningJob)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    JobGate gate;
    idx.set_job_hook(gate.hook());
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    gate.entered.get_future().wait();
    idx.ensure(path, config_for(path), IndexLevel::Request);
    idx.ensure(path, config_for(path), IndexLevel::Request);
    EXPECT_EQ(idx.stats().pending, 1); // only the running job
    gate.release.set_value();
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(snap.missing.empty());
    EXPECT_NE(snap.entries.at(path)->text.find("int f()"), std::string::npos);
    EXPECT_EQ(idx.stats().analyzed, 1); // no second job
}

// HOT-34: ensure() creates a slot for an unknown path and queues a job once.
TEST(WorkspaceIndex, EnsureCreatesASlotAndKeepsAQueuedJob)
{
    WorkspaceIndex idx;
    idx.ensure("a.hot", JobConfig{}, IndexLevel::Request);
    EXPECT_EQ(idx.stats().total, 1);
    EXPECT_EQ(idx.stats().pending, 1);
    idx.ensure("a.hot", JobConfig{}, IndexLevel::Request);
    EXPECT_EQ(idx.stats().pending, 1);
    EXPECT_EQ(idx.stats().total, 1);
}

// HOT-34: known_paths() gives each path that has a slot with a config.
TEST(WorkspaceIndex, KnownPathsHaveAConfig)
{
    WorkspaceIndex idx;
    idx.schedule("b.hot", JobConfig{}, IndexLevel::Rest);
    idx.ensure("a.hot", JobConfig{}, IndexLevel::Request);
    idx.set("c.hot", entry_with_deps("c.hot", {})); // a slot with no config
    EXPECT_EQ(idx.known_paths(), (std::vector<std::string>{"a.hot", "b.hot"}));
    idx.forget("a.hot");
    EXPECT_EQ(idx.known_paths(), (std::vector<std::string>{"b.hot"}));
}

// HOT-34: a path with a valid entry and no job: ensure() queues nothing.
TEST(WorkspaceIndex, EnsureQueuesNothingForAValidEntry)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    std::atomic<int> jobs{0};
    idx.set_job_hook([&](const std::string&) { ++jobs; });
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    ASSERT_TRUE(snap.missing.empty());
    while (idx.stats().pending != 0)
        std::this_thread::yield();
    idx.ensure(path, config_for(path), IndexLevel::Request);
    EXPECT_EQ(idx.stats().pending, 0);
    idx.stop();
    EXPECT_EQ(jobs.load(), 1);
}

// HOT-34: forget() and schedule() of a path in the middle of a job. The old result is dropped.
TEST(WorkspaceIndex, ForgetAndScheduleDuringAJobDropTheOldResult)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    JobGate gate;
    idx.set_job_hook(gate.hook());
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    gate.entered.get_future().wait(); // the job has read kTextOne
    tmp.write("main.hot", kTextTwo, -50);
    idx.forget(path);
    idx.schedule(path, config_for(path), IndexLevel::Request);
    gate.release.set_value();
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(snap.missing.empty());
    EXPECT_NE(snap.entries.at(path)->text.find("int g()"), std::string::npos);
}

// HOT-34: a file changes after the job read it and before the publish, inside one wait_for.
// The snapshot must not hold the old text.
TEST(WorkspaceIndex, WaitForRejectsAnEntryPublishedWithAnOldStamp)
{
    TempDir tmp;
    const std::string path = tmp.write("main.hot", kTextOne, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    JobGate gate;
    idx.set_job_hook(
        [&](const std::string& p)
        {
            if (gate.calls++ == 0)
                tmp.write("main.hot", kTextTwo, -50); // after the read, before the publish
            (void)p;
        });
    idx.schedule(path, config_for(path), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto snap = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(snap.missing.empty());
    EXPECT_NE(snap.entries.at(path)->text.find("int g()"), std::string::npos);
    EXPECT_GE(idx.stats().analyzed, 2);
}

// HOT-34: build_entry keeps the dep stamps that the caller gives. It does not stat again.
TEST(WorkspaceIndex, BuildEntryUsesTheGivenDepStamps)
{
    TempDir tmp;
    const std::string text = "class A\n{\n    int f() { return 1; }\n}\n";
    const std::string path = tmp.write("a.hot", text, -100);
    SymbolIndex index;
    hotc::TranslatorSession session;
    const std::vector<SymbolIndex::DepStamp> deps = {{(tmp.dir / "x.hot").string(), 7, 9}};
    const auto e = build_entry(path, stamp_now(path), text, options_for(path), "d", deps, index, session);
    ASSERT_TRUE(e);
    ASSERT_EQ(e->dep_stamps.size(), 1u);
    EXPECT_EQ(e->dep_stamps[0].first, deps[0].path);
    EXPECT_EQ(e->dep_stamps[0].second, (FileStamp{7, 9}));
}

// HOT-34: a dep stamp keeps the spelling. files_with_dep uses the normal form of the path.
TEST(WorkspaceIndex, DepPathsHaveTheNormalForm)
{
    TempDir tmp;
    fs::create_directories(tmp.dir / "sub");
    const std::string lib = tmp.write("lib.hot", "int helper() { return 1; }\n", -100);
    const std::string text = "using \"sub/../lib.hot\";\nclass M\n{\n    int f() { return helper(); }\n}\n";
    const std::string path = tmp.write("main.hot", text, -100);
    SymbolIndex index;
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), text, options_for(path), "d", index, session);
    ASSERT_TRUE(e);
    ASSERT_EQ(e->dep_stamps.size(), 1u);
    EXPECT_EQ(e->dep_stamps[0].first, (tmp.dir / "sub/../lib.hot").string());
    EXPECT_TRUE(fresh(*e));
    WorkspaceIndex idx;
    idx.set(path, e);
    EXPECT_EQ(idx.files_with_dep(lib), (std::vector<std::string>{path}));
    EXPECT_TRUE(idx.files_with_dep(e->dep_stamps[0].first).empty());
    idx.erase(path);
    EXPECT_TRUE(idx.files_with_dep(lib).empty());
}

// HOT-34: an include dir of the form "a/../x" keeps its spelling in the stamp. by_dep_ has the
// normal key.
TEST(WorkspaceIndex, IncludeDirWithDotDotKeepsTheSpelling)
{
    TempDir tmp;
    fs::create_directories(tmp.dir / "a");
    fs::create_directories(tmp.dir / "x");
    const std::string lib = tmp.write("x/lib.hot", "int helper() { return 1; }\n", -100);
    const std::string text = "using \"lib.hot\";\nclass M\n{\n    int f() { return helper(); }\n}\n";
    const std::string path = tmp.write("main.hot", text, -100);
    SymbolIndex index;
    hotc::TranslatorSession session;
    hotc::TranslateOptions opts = options_for(path);
    const std::string dir = (tmp.dir / "a/../x").string();
    opts.include_dirs = {dir};
    std::vector<SymbolIndex::DepStamp> deps;
    const std::string digest = index.cached_digest(path, text, opts.include_dirs, &deps);
    const auto e = build_entry(path, stamp_now(path), text, opts, digest, deps, index, session);
    ASSERT_TRUE(e);
    bool found = false;
    for (const auto& [file, stamp] : e->dep_stamps)
        if (file == (fs::path(dir) / "lib.hot").string())
            found = true;
    EXPECT_TRUE(found);
    EXPECT_TRUE(fresh(*e));
    WorkspaceIndex idx;
    idx.set(path, e);
    EXPECT_EQ(idx.files_with_dep(fs::path(lib).lexically_normal().string()), (std::vector<std::string>{path}));
}

#ifndef _WIN32
// HOT-34: "link/../lib.hot" with a symlink names another file than the lexical form. The stat
// follows the spelling, so fresh() stays true when nothing changes.
TEST(WorkspaceIndex, SymlinkInADepPathStaysFresh)
{
    TempDir tmp;
    fs::create_directories(tmp.dir / "real/sub");
    std::error_code ec;
    fs::create_directory_symlink(tmp.dir / "real/sub", tmp.dir / "link", ec);
    if (ec)
        GTEST_SKIP() << "no symlink";
    tmp.write("real/lib.hot", "int helper() { return 1; }\n", -100);
    const std::string text = "using \"link/../lib.hot\";\nclass M\n{\n    int f() { return helper(); }\n}\n";
    const std::string path = tmp.write("main.hot", text, -100);
    SymbolIndex index;
    hotc::TranslatorSession session;
    const auto e = build_here(path, stamp_now(path), text, options_for(path), "d", index, session);
    ASSERT_TRUE(e);
    ASSERT_EQ(e->dep_stamps.size(), 1u);
    EXPECT_NE(e->dep_stamps[0].second.size, UINT64_MAX);
    EXPECT_TRUE(fresh(*e));
    EXPECT_TRUE(fresh(*e));
}
#endif

// ---- HOT-34: the preludes in the worker

namespace
{
const char* const kPreludeOne = "int helper() { return 1; }\n";
const char* const kPreludeTwo = "int helper() { return 2; }\n";
const char* const kPreludeScript = "class M\n{\n    int f() { return helper(); }\n}\n";

std::string prelude_key(const std::string& p) { return fs::path(p).lexically_normal().string(); }

JobConfig config_with_prelude(const std::string& path, const std::string& prelude)
{
    JobConfig c = config_for(path);
    c.prelude_paths = {prelude_key(prelude)};
    return c;
}

// Runs one index with one worker. Gives the entry of the script.
std::shared_ptr<const Entry> index_once(const JobConfig& c, const std::string& path, SymbolIndex& symbols)
{
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    idx.schedule(path, c, IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto r = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    return r.missing.empty() ? r.entries.at(path) : nullptr;
}

bool has_dep(const Entry& e, const std::string& dep, bool missing)
{
    for (const auto& [file, stamp] : e.dep_stamps)
        if (file == dep)
            return (stamp.size == UINT64_MAX) == missing;
    return false;
}
} // namespace

// HOT-34: a prelude is a dep of the script. A prelude edit makes the entry old.
TEST(WorkspaceIndex, PreludeIsADepAndAnEditMakesTheEntryOld)
{
    TempDir tmp;
    const std::string prelude = tmp.write("p.hot", kPreludeOne, -100);
    const std::string path = tmp.write("main.hot", kPreludeScript, -100);
    SymbolIndex symbols;
    hotc::TranslatorSession session;
    WorkspaceIndex idx;
    idx.schedule(path, config_with_prelude(path, prelude), IndexLevel::Rest);
    idx.finish_scheduling();
    idx.start(symbols, session, 1, nullptr);
    const auto first = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    ASSERT_TRUE(first.missing.empty());
    const auto old = first.entries.at(path);
    EXPECT_TRUE(has_dep(*old, prelude_key(prelude), false));
    EXPECT_EQ(idx.files_with_dep(prelude_key(prelude)), (std::vector<std::string>{path}));
    EXPECT_TRUE(fresh(*old, nullptr));

    tmp.write("p.hot", kPreludeTwo, -50);
    EXPECT_FALSE(fresh(*old, nullptr));
    idx.invalidate(prelude_key(prelude)); // the prelude is no script: only the importer queues
    const auto second = idx.wait_for({path}, nullptr, std::chrono::steady_clock::now() + kLong);
    idx.stop();
    ASSERT_TRUE(second.missing.empty());
    const auto now = second.entries.at(path);
    EXPECT_NE(now, old);
    EXPECT_NE(now->digest, old->digest);
    EXPECT_TRUE(fresh(*now, nullptr));
}

// HOT-34: a missing prelude has the missing stamp. A new file makes the entry old.
TEST(WorkspaceIndex, MissingPreludeHasAMissingStampAndCreatingItMakesTheEntryOld)
{
    TempDir tmp;
    const std::string prelude = (tmp.dir / "p.hot").string();
    const std::string path = tmp.write("main.hot", "class M\n{\n    int f() { return 1; }\n}\n", -100);
    SymbolIndex symbols;
    const auto e = index_once(config_with_prelude(path, prelude), path, symbols);
    ASSERT_TRUE(e);
    EXPECT_TRUE(has_dep(*e, prelude_key(prelude), true));
    EXPECT_TRUE(fresh(*e, nullptr));
    tmp.write("p.hot", kPreludeOne);
    EXPECT_FALSE(fresh(*e, nullptr));
}

// HOT-34: the prelude text is in the digest. An old shard does not load for a new prelude.
TEST(WorkspaceIndex, PreludeTextChangesTheDigestAndTheShardDoesNotLoad)
{
    TempDir tmp;
    const std::string prelude = tmp.write("p.hot", kPreludeOne, -100);
    const std::string path = tmp.write("main.hot", kPreludeScript, -100);
    const JobConfig c = config_with_prelude(path, prelude);
    SymbolIndex symbols;
    symbols.open((tmp.dir / "shards").string());
    const auto a = index_once(c, path, symbols);
    ASSERT_TRUE(a);
    EXPECT_GT(symbols.stores(), 0);
    const int loads = symbols.loads();
    const auto same = index_once(c, path, symbols);
    ASSERT_TRUE(same);
    EXPECT_EQ(same->digest, a->digest);
    EXPECT_GT(symbols.loads(), loads); // the same prelude text: the shard loads

    tmp.write("p.hot", kPreludeTwo, -50);
    const int loads2 = symbols.loads();
    const auto b = index_once(c, path, symbols);
    ASSERT_TRUE(b);
    EXPECT_NE(b->digest, a->digest);
    EXPECT_EQ(symbols.loads(), loads2); // the old shard does not load
}

// HOT-34: the prelude key has the form of index_key(), the dep map key.
TEST(WorkspaceIndex, PreludeKeyHasTheFormOfIndexKey)
{
    TempDir tmp;
    fs::create_directories(tmp.dir / "sub");
    const std::string prelude = tmp.write("p.hot", kPreludeOne, -100);
    const std::string path = tmp.write("main.hot", kPreludeScript, -100);
    const std::string spelled = (tmp.dir / "sub" / ".." / "p.hot").string();
    SymbolIndex symbols;
    const auto e = index_once(config_with_prelude(path, spelled), path, symbols);
    ASSERT_TRUE(e);
    ASSERT_EQ(e->dep_stamps.size(), 1u);
    EXPECT_EQ(e->dep_stamps[0].first, fs::path(prelude).lexically_normal().string());
    WorkspaceIndex idx;
    idx.set(path, e);
    EXPECT_EQ(idx.files_with_dep(fs::path(prelude).lexically_normal().string()), (std::vector<std::string>{path}));
}
