// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "workspace_index.h"

#include "long_path.h"
#include "script_text.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hot::ls
{

void WorkspaceIndex::add_locked(const std::string& path, const Entry& e)
{
    for (const std::string& w : e.words)
        by_word_[w].insert(path);
    for (const std::string& w : e.unclaimed_words)
        by_unclaimed_[w].insert(path);
    if (e.table)
        for (const hotc::ResolvedRef& r : e.table->refs)
            if (!r.id.empty())
                by_ref_[r.id].insert(path);
}

void WorkspaceIndex::remove_locked(const std::string& path, const Entry& e)
{
    const auto drop = [&](FileMap& m, const std::string& key)
    {
        const auto it = m.find(key);
        if (it == m.end())
            return;
        it->second.erase(path);
        if (it->second.empty())
            m.erase(it);
    };
    for (const std::string& w : e.words)
        drop(by_word_, w);
    for (const std::string& w : e.unclaimed_words)
        drop(by_unclaimed_, w);
    if (e.table)
        for (const hotc::ResolvedRef& r : e.table->refs)
            if (!r.id.empty())
                drop(by_ref_, r.id);
}

void WorkspaceIndex::set(const std::string& path, std::shared_ptr<const Entry> entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    set_locked(path, std::move(entry));
}

void WorkspaceIndex::set_locked(const std::string& path, std::shared_ptr<const Entry> entry)
{
    const auto it = entries_.find(path);
    if (it != entries_.end())
    {
        remove_locked(path, *it->second);
        entries_.erase(it);
    }
    if (!entry)
        return;
    add_locked(path, *entry);
    entries_[path] = std::move(entry);
}

void WorkspaceIndex::erase(const std::string& path) { set(path, nullptr); }

std::shared_ptr<const Entry> WorkspaceIndex::entry(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : it->second;
}

std::vector<std::string> WorkspaceIndex::lookup(const FileMap& m, const std::string& key)
{
    const auto it = m.find(key);
    if (it == m.end())
        return {};
    return std::vector<std::string>(it->second.begin(), it->second.end());
}

std::vector<std::string> WorkspaceIndex::files_with_word(const std::string& word) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(by_word_, word);
}

std::vector<std::string> WorkspaceIndex::files_with_unclaimed_word(const std::string& word) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(by_unclaimed_, word);
}

std::vector<std::string> WorkspaceIndex::files_with_ref(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lookup(by_ref_, id);
}

std::vector<std::shared_ptr<const Entry>> WorkspaceIndex::all_entries() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<const Entry>> out;
    out.reserve(entries_.size());
    for (const auto& [path, e] : entries_)
        out.push_back(e);
    return out;
}

bool stat_file(const std::string& path, FileStamp& out)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec)
        return false;
    const auto time = fs::last_write_time(path, ec);
    if (ec)
        return false;
    out.size = static_cast<uint64_t>(size);
    out.mtime = static_cast<int64_t>(time.time_since_epoch().count());
    return true;
}

namespace
{
// The stamp that stands for a file that is missing. It is the same value as in SymbolIndex.
constexpr uint64_t kMissingSize = UINT64_MAX;

FileStamp stat_or_missing(const std::string& file, std::map<std::string, FileStamp>* memo)
{
    if (memo)
    {
        const auto it = memo->find(file);
        if (it != memo->end())
            return it->second;
    }
    FileStamp s;
    if (!stat_file(file, s))
    {
        s.size = kMissingSize;
        s.mtime = 0;
    }
    if (memo)
        (*memo)[file] = s;
    return s;
}
} // namespace

bool fresh(const Entry& e, std::map<std::string, FileStamp>* memo)
{
    const FileStamp self = stat_or_missing(e.path, memo);
    if (self.size == kMissingSize || self != e.stamp)
        return false;
    for (const auto& [file, stamp] : e.dep_stamps)
        if (stat_or_missing(file, memo) != stamp)
            return false;
    return true;
}

std::shared_ptr<const Entry> build_entry(const std::string& path, const FileStamp& stamp, const std::string& text,
    const hotc::TranslateOptions& options, const std::string& digest, SymbolIndex& index,
    hotc::TranslatorSession& session, bool* analyzed)
{
    if (analyzed)
        *analyzed = false;
    auto e = std::make_shared<Entry>();
    e->path = path;
    e->digest = digest;
    e->text = text;
    e->stamp = stamp; // the caller took it before it read the text
    // The digest search stamps each probed file before it reads the file. It also stamps a
    // missing candidate, so a file that appears earlier in the search makes fresh() give false.
    std::vector<SymbolIndex::DepStamp> deps;
    index.cached_digest(path, text, options.include_dirs, &deps);
    for (const auto& d : deps)
        e->dep_stamps.emplace_back(d.path, FileStamp{d.size, d.mtime});
    auto shard = index.load(path, digest);
    if (!shard)
    {
        if (analyzed)
            *analyzed = true;
        const hotc::AnalyzeResult facts = hotc::analyze(text, options, session);
        shard = shard_of_facts(path, Text(text), facts, script_imports_in(options, path, text));
        index.store(path, digest, *shard);
    }
    e->table = std::make_shared<const FileRefs>(std::move(shard->table));
    e->classes = std::move(shard->classes);
    collect_words(*e->table, text, e->words, e->unclaimed_words);
    return e;
}

// ------------------------------------------------------------------ the background indexer

namespace
{
void lower_thread_priority()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
}

bool read_whole(const std::string& path, std::string& out)
{
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}
} // namespace

void WorkspaceIndex::start(SymbolIndex& symbols, hotc::TranslatorSession& session, unsigned threads,
    std::function<void()> notify)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_.load() || stop_)
        return;
    symbols_ = &symbols;
    session_ = &session;
    notify_ = std::move(notify);
    if (threads == 0)
        threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    threads_ = static_cast<int>(threads);
    started_ = true;
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this] { worker_main(); });
}

void WorkspaceIndex::stop()
{
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        workers.swap(workers_);
    }
    work_cv_.notify_all();
    done_cv_.notify_all();
    for (std::thread& t : workers)
        if (t.joinable())
            t.join();
}

void WorkspaceIndex::schedule(const std::string& path, JobConfig config, IndexLevel level)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
            return;
        if (!scheduled_.exchange(true))
            first_schedule_ = std::chrono::steady_clock::now();
        known_.insert(path);
        total_ = static_cast<int>(known_.size());
        auto it = jobs_.find(path);
        if (it == jobs_.end())
        {
            jobs_[path] = Job{std::move(config), level};
            queues_[static_cast<int>(level)].push_back(path);
        }
        else
        {
            it->second.config = std::move(config);
            if (level < it->second.level)
            {
                it->second.level = level;
                queues_[static_cast<int>(level)].push_back(path);
            }
        }
        pending_ = static_cast<int>(jobs_.size() + running_.size());
        check_ready_locked();
    }
    work_cv_.notify_one();
}

void WorkspaceIndex::finish_scheduling()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
            return;
        if (!scheduled_.exchange(true))
            first_schedule_ = std::chrono::steady_clock::now();
        check_ready_locked();
    }
    done_cv_.notify_all();
}

void WorkspaceIndex::bump(const std::vector<std::string>& paths)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& path : paths)
    {
        const auto it = jobs_.find(path);
        if (it == jobs_.end() || it->second.level == IndexLevel::Request)
            continue;
        it->second.level = IndexLevel::Request;
        queues_[0].push_back(path);
    }
}

void WorkspaceIndex::invalidate(const std::string& path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_[path];
}

std::vector<std::string> WorkspaceIndex::wait_for(const std::vector<std::string>& paths,
    const std::function<void()>& poll, std::chrono::steady_clock::time_point deadline)
{
    bump(paths);
    work_cv_.notify_all();
    for (;;)
    {
        std::vector<std::string> missing;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (const std::string& path : paths)
                if (!entries_.count(path) && (jobs_.count(path) || running_.count(path)))
                    missing.push_back(path);
            if (missing.empty() || stop_ || std::chrono::steady_clock::now() >= deadline)
                return missing;
            done_cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (poll)
            poll();
    }
}

IndexStats WorkspaceIndex::stats() const
{
    IndexStats s;
    s.total = total_.load();
    s.done = done_.load();
    s.pending = pending_.load();
    s.analyzed = analyzed_.load();
    s.loaded = loaded_.load();
    s.ready_ms = ready_ms_.load();
    s.threads = threads_.load();
    s.ready = scheduled_.load() && s.done >= s.total;
    return s;
}

void WorkspaceIndex::check_ready_locked()
{
    done_ = static_cast<int>(finished_.size());
    if (scheduled_.load() && finished_.size() >= known_.size() && ready_ms_.load() < 0)
        ready_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - first_schedule_).count();
}

bool WorkspaceIndex::take(std::string& path, Job& job, uint64_t& generation)
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;)
    {
        if (stop_)
            return false;
        for (int level = 0; level < 3; ++level)
        {
            std::deque<std::string>& q = queues_[level];
            for (auto it = q.begin(); it != q.end();)
            {
                const auto j = jobs_.find(*it);
                // A stale entry: the job ran, or it moved to a lower level number.
                if (j == jobs_.end() || static_cast<int>(j->second.level) != level)
                {
                    it = q.erase(it);
                    continue;
                }
                if (running_.count(*it))
                {
                    ++it; // one job for a path at a time
                    continue;
                }
                path = *it;
                job = std::move(j->second);
                jobs_.erase(j);
                q.erase(it);
                running_.insert(path);
                generation = generation_[path];
                return true;
            }
        }
        work_cv_.wait(lock);
    }
}

void WorkspaceIndex::finish(const std::string& path, Job&& job, uint64_t generation,
    std::shared_ptr<const Entry> entry, bool analyzed)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.erase(path);
        if (analyzed)
            ++analyzed_;
        else if (entry)
            ++loaded_;
        if (generation_[path] != generation && !stop_)
        {
            // The path changed during the job: drop the result and run the job again.
            if (!jobs_.count(path))
            {
                const int level = static_cast<int>(job.level);
                jobs_[path] = std::move(job);
                queues_[level].push_back(path);
            }
        }
        else
        {
            if (entry)
                set_locked(path, std::move(entry));
            finished_.insert(path); // a failed job has no entry, but nobody waits for it
        }
        pending_ = static_cast<int>(jobs_.size() + running_.size());
        check_ready_locked();
    }
    done_cv_.notify_all();
    work_cv_.notify_one(); // a queued job of this path can run now
}

void WorkspaceIndex::worker_main()
{
    lower_thread_priority();
    std::string path;
    Job job;
    uint64_t generation = 0;
    while (take(path, job, generation))
    {
        std::shared_ptr<const Entry> entry;
        bool analyzed = false;
        try
        {
            std::string text;
            FileStamp stamp; // taken before the read: a change after it makes the stamp old
            if (stat_file(path, stamp) && read_whole(path, text))
            {
                const JobConfig& c = job.config;
                const std::string digest = SymbolIndex::digest(
                    symbols_->cached_digest(path, text, c.options.include_dirs) + c.flags_text
                    + "\nbuild:" + c.build_id);
                entry = build_entry(path, stamp, text, c.options, digest, *symbols_, *session_, &analyzed);
            }
        }
        catch (...)
        {
            entry = nullptr;
        }
        finish(path, std::move(job), generation, std::move(entry), analyzed);
        if (notify_)
            notify_();
    }
}

} // namespace hot::ls
