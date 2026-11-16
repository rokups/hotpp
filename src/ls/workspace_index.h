// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The workspace index of hot-ls (ticket HOT-34). It keeps one immutable Entry for each script
// and the inverted maps over all entries: word to files, unclaimed word to files, and ref id to
// files. A later step lets the server and the background threads use it.
//
// The class has the maps and the background indexer. The indexer has worker threads at low
// priority. A worker uses only its JobConfig and the objects that start() gets. It never
// touches the server.
// The class is safe for many threads. One mutex guards the entries and the maps. No function
// holds the mutex during hotc::analyze, a SymbolIndex call or a file read. The lock order is:
// WorkspaceIndex::mutex_ first, then the locks of SymbolIndex.
#pragma once

#include "hot/hotc_lib.h"
#include "ref_resolver.h"
#include "symbol_index.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hot::ls
{

// The size and the modification time of a file.
struct FileStamp
{
    uint64_t size = 0;
    int64_t mtime = 0;
    bool operator==(const FileStamp& o) const { return size == o.size && mtime == o.mtime; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

// The data of one script. An Entry does not change after set() publishes it. The index stores
// it as std::shared_ptr<const Entry>.
// A later native identity (HOT-35) adds one field here and one map to the index.
struct Entry
{
    std::string path;
    FileStamp stamp; // of the script
    // The stamp of each file in the import closure of the script, in the order of the search.
    std::vector<std::pair<std::string, FileStamp>> dep_stamps;
    std::string digest; // the shard digest of the text
    std::string text;
    std::shared_ptr<const FileRefs> table;
    std::vector<IndexClass> classes;
    std::set<std::string> words; // every identifier word of the text
    // The code words with one or more occurrences that are not an exact resolved ref. This
    // includes unresolved names and the words of raw bodies.
    std::set<std::string> unclaimed_words;
};

// The immutable snapshot of what a worker needs to index one script: the translate options
// without the text, the flags text and the build id for the digest. The state thread makes it.
struct JobConfig
{
    hotc::TranslateOptions options; // script_text is empty, preludes is empty
    std::vector<std::string> prelude_paths; // the prelude files in the form of index_key(); the worker reads them
    std::string flags_text;
    std::string build_id;
};

// The levels of the queue. A lower number runs first.
enum class IndexLevel
{
    Request = 0, // a request waits for the script
    Importer = 1, // an importer of a changed file
    Rest = 2,
};

// The numbers of the indexer. They come from atomics, so any thread can read them.
struct IndexStats
{
    bool ready = false; // every scheduled script has an entry or failed
    int total = 0, done = 0, pending = 0, analyzed = 0, loaded = 0;
    int64_t ready_ms = -1; // the time from the first schedule to ready, or -1
    int threads = 0;
};

// The result of WorkspaceIndex::wait_for.
struct IndexSnapshot
{
    // The valid entries only. An entry is valid when it is of the current generation of the
    // slot and it passed the stamp check in the call, or a worker published it in the call.
    std::map<std::string, std::shared_ptr<const Entry>> entries;
    // The paths that are stale at the deadline, that failed, or that the index does not know.
    std::vector<std::string> missing;
};

class WorkspaceIndex
{
public:
    WorkspaceIndex() = default;
    ~WorkspaceIndex() { stop(); }
    WorkspaceIndex(const WorkspaceIndex&) = delete;
    WorkspaceIndex& operator=(const WorkspaceIndex&) = delete;

    // Starts the worker threads. "threads" 0 gives max(1, hardware_concurrency() / 2).
    // "notify" runs on a worker after each finished job. It must be thread safe.
    // "symbols" and "session" must live until stop() returns.
    void start(SymbolIndex& symbols, hotc::TranslatorSession& session, unsigned threads,
        std::function<void()> notify);
    // Sets the stop flag, wakes the workers and joins them. Safe to call many times.
    void stop();
    bool started() const { return started_.load(); }

    // Queues the script. A script that is queued already keeps the lower level and gets the
    // new config. Only the state thread calls it.
    void schedule(const std::string& path, JobConfig config, IndexLevel level);
    // Like schedule(), for a path that a request needs. It never drops a running job: when the
    // path has a queued or a running job, it only moves a queued job to a lower level and it
    // does not count a change. A path with a valid entry and no job gets nothing. Else it does
    // schedule(). Only the state thread calls it.
    void ensure(const std::string& path, JobConfig config, IndexLevel level);
    // Tells the index that the first scheduling ends. An index with no script is ready after
    // this call. Only the state thread calls it.
    void finish_scheduling();
    // Moves the queued jobs of the paths to the level 0.
    void bump(const std::vector<std::string>& paths);
    // Counts a change of the path and of its importers, and queues a job for each one at the Importer level.
    void invalidate(const std::string& path);
    // Removes the entry, the slot and the queued job of a deleted script.
    void forget(const std::string& path);
    // Waits until each path has a valid entry (see IndexSnapshot), failed or is not known. It
    // checks the stamps of the entries on the caller thread without the lock, with one stat of
    // each file in the call. It queues a job for a stale entry and bumps the jobs of the paths
    // in each round. A slot with no config (made by set() only) gets no job: its stale path is
    // missing. Calls "poll" about each 50 ms without a lock (it may throw). A deadline in the
    // past returns after one round.
    IndexSnapshot wait_for(const std::vector<std::string>& paths,
        const std::function<void()>& poll, std::chrono::steady_clock::time_point deadline);
    IndexStats stats() const;
    // A test hook. The worker calls it after the read and the build and before finish(). Set
    // it before start(). The default is empty.
    void set_job_hook(std::function<void(const std::string&)> hook) { job_hook_ = std::move(hook); }

    // Publishes the entry of "path". It replaces the old entry and updates the maps.
    void set(const std::string& path, std::shared_ptr<const Entry> entry);
    void erase(const std::string& path);
    // The entry of "path", or null.
    std::shared_ptr<const Entry> entry(const std::string& path) const;
    // The sorted paths of the files that have the word in the text.
    std::vector<std::string> files_with_word(const std::string& word) const;
    // The sorted paths of the files where the word has an occurrence that is not an exact
    // resolved ref. A later native identity (HOT-35) adds one more map like this one.
    std::vector<std::string> files_with_unclaimed_word(const std::string& word) const;
    // The sorted paths of the files that have a ref with the id (ResolvedRef::id).
    std::vector<std::string> files_with_ref(const std::string& id) const;
    // The sorted paths of the scripts that have "dep" in Entry::dep_stamps. A missing
    // candidate counts too.
    std::vector<std::string> files_with_dep(const std::string& dep) const;
    std::vector<std::shared_ptr<const Entry>> all_entries() const;

private:
    struct Job // what a worker runs: the config comes from the slot
    {
        JobConfig config;
        IndexLevel level = IndexLevel::Rest;
    };
    // Counts a change of the path and queues a job at "level" (keeps a higher priority level).
    void queue_locked(const std::string& path, IndexLevel level);
    // Does invalidate() for the lock holder. Gives true when a job is queued for "path" itself.
    bool invalidate_locked(const std::string& path);
    // Like invalidate(), but only when entries_[path] is still "stale". Gives false when the
    // path gets no job. Gives true when a newer entry replaced "stale".
    bool invalidate_stale(const std::string& path, const std::shared_ptr<const Entry>& stale);
    void set_locked(const std::string& path, std::shared_ptr<const Entry> entry, uint64_t generation);
    // Does the work of schedule() for the lock holder.
    void schedule_locked(const std::string& path, JobConfig config, IndexLevel level);
    void worker_main();
    // Takes the next job that no worker runs. False at stop.
    bool take(std::string& path, Job& job, uint64_t& generation);
    void finish(const std::string& path, const Job& job, uint64_t generation,
        std::shared_ptr<const Entry> entry, bool analyzed);
    void check_ready_locked();
    using FileMap = std::map<std::string, std::set<std::string>>;
    void add_locked(const std::string& path, const Entry& e);
    void remove_locked(const std::string& path, const Entry& e);
    static std::vector<std::string> lookup(const FileMap& m, const std::string& key);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const Entry>> entries_;
    FileMap by_word_, by_unclaimed_, by_ref_;
    FileMap by_dep_; // dependency file -> the scripts that have this file in Entry::dep_stamps

    // The indexer. mutex_ guards the queue and the sets too.
    SymbolIndex* symbols_ = nullptr;
    hotc::TranslatorSession* session_ = nullptr;
    std::function<void()> notify_;
    std::function<void(const std::string&)> job_hook_;
    std::vector<std::thread> workers_;
    std::condition_variable work_cv_, done_cv_;
    bool stop_ = false;
    std::atomic<bool> started_{false};
    std::map<std::string, IndexLevel> jobs_; // the queued jobs by path
    std::deque<std::string> queues_[3]; // the paths by level; an entry can be stale
    std::set<std::string> running_, known_, finished_;
    struct Slot // one for each known path
    {
        JobConfig config;              // kept after take(), so the index can queue the path again
        bool has_config = false;       // schedule() set the config; a slot without it gets no job
        uint64_t generation = 0;       // a value of next_generation_; 0 when no change happened
        uint64_t entry_generation = 0; // the generation at the start of the job that made the entry
        bool failed = false;           // the last job at this generation gave no entry
    };
    std::map<std::string, Slot> slots_;
    uint64_t next_generation_ = 0; // the source of every generation; it never gives a value twice
    std::chrono::steady_clock::time_point first_schedule_;
    std::atomic<int> total_{0}, done_{0}, pending_{0}, analyzed_{0}, loaded_{0}, threads_{0};
    std::atomic<int64_t> ready_ms_{-1};
    std::atomic<bool> scheduled_{false};
};

// The stamp of a file. Gives false when the file is missing.
bool stat_file(const std::string& path, FileStamp& out);

// True when the files still have the stamps of the entry. It does a stat of the script and of
// each dependency. It reads no file. A changed stamp gives false. A dependency that was missing
// and is still missing is equal. "memo" (if given) keeps the stamps for the life of one call
// series: the caller makes it empty and drops it after the pass.
bool fresh(const Entry& e, std::map<std::string, FileStamp>* memo = nullptr);

// Builds the entry of a script. It is pure: it reads no server state. The shard comes from
// "index" when the digest matches, else from hotc::analyze, and it goes into "index". The
// imports are read from the disk only, with "options.fs" when it is set. "stamp" is the stamp
// of the script that the caller took before it read "text". "deps" are the stamps that the
// same cached_digest call gave for "digest". The caller makes both with one call, so the
// entry has the stamps of the imports that the digest has. A change after the read makes
// fresh() give false. "analyzed" (if given) gets true when the call ran hotc::analyze, false
// when it loaded the shard.
std::shared_ptr<const Entry> build_entry(const std::string& path, const FileStamp& stamp, const std::string& text,
    const hotc::TranslateOptions& options, const std::string& digest,
    const std::vector<SymbolIndex::DepStamp>& deps, SymbolIndex& index, hotc::TranslatorSession& session,
    bool* analyzed = nullptr);

} // namespace hot::ls
