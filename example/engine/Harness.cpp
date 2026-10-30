#include "Harness.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace harness {

Command parse_command(const std::string& line)
{
    std::istringstream in(line);
    std::string word;
    if (!(in >> word)) return Command::Empty;
    if (word == "reload") return Command::Reload;
    if (word == "stats" || word == "script_stats") return Command::Stats;
    if (word == "quit" || word == "exit") return Command::Quit;
    return Command::Unknown;
}

Harness::Harness(std::string script, std::string class_name)
    : script_(std::move(script)),
      class_name_(std::move(class_name)),
      backend_(spp::make_default_backend()),
      watcher_(script_)
{
}

static spp::ScriptSource read_source(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return {path, ss.str()};
}

bool Harness::start(std::string& error)
{
    spp::LoadResult r = backend_->load(read_source(script_), next_generation_++);
    if (!r) {
        error = r.error;
        return false;
    }
    if (const spp::ReloadResult loaded = loader_.load(std::move(*r.module)); !loaded) {
        error = loaded.error;
        return false;
    }
    Actor* first = make_actor();
    if (!first) {
        error = "class " + class_name_ + " is not registered";
        return false;
    }
    world_.Add(first);
    world_.SetFrameBoundaryHook([this] {
        if (loader_.reload_pending()) {
            const spp::ReloadResult committed = loader_.commit_pending();
            if (!committed && !committed.error.empty())
                std::fprintf(stderr, "[harness] commit failed: %s\n", committed.error.c_str());
        }
        loader_.frame_boundary();
    });
    return true;
}

Actor* Harness::make_actor()
{
    const spp::ClassEntry* entry = loader_.find(class_name_);
    return entry ? static_cast<Actor*>(entry->create().native) : nullptr;
}

spp::ReloadResult Harness::reload()
{
    ++reloads_;
    const spp::ScriptSource source = read_source(script_);  // the file is read once for each reload
    // A full reload failed after the reset: the Loader has no script and the actors wait in
    // "saved_". Try the full reload again, whatever the file says now.
    if (restore_pending_) return full_reload(source, {});
    // The Loader overload maps a backend failure to a result. A build failure with the reason
    // SlotsRemoved becomes FullReloadRequired. So the Harness needs no own mapping.
    spp::ReloadResult result = loader_.request_reload(backend_->load(source, next_generation_++));
    if (result.status == spp::ReloadStatus::FullReloadRequired) return full_reload(source, result);
    return result;
}

// The full reload sequence of the host. The Loader cannot apply the change, so the host
// restarts all script state and keeps its own state. The Fresh build comes first: it changes no
// Loader state and no slots file, so a build error leaves the old scripts and the actors as they are.
spp::ReloadResult Harness::full_reload(const spp::ScriptSource& source, const spp::ReloadResult& needed)
{
    if (!restore_pending_) std::printf("[host] full reload: %s\n", needed.error.c_str());
    spp::ReloadResult result;
    result.reason = needed.reason;
    result.classes = needed.classes;

    spp::LoadResult fresh = backend_->load(source, 1, spp::LoadMode::Fresh);  // (a)
    if (!fresh) {
        result.error = "full reload failed: " + fresh.error;
        if (restore_pending_) result.error += "\n(the saved actors stay for the next reload)";
        return result;
    }
    if (!restore_pending_) {
        saved_ = world_.Save();  // (b) the host state
        world_.Clear();          // (c) no script object, handle or closure remains
        std::string error;
        if (!loader_.reset(error)) {  // (d) the Loader is unchanged: the old scripts go on
            world_.Restore(saved_, [this] { return make_actor(); });
            saved_.clear();
            result.error = "full reload failed: " + error;
            return result;
        }
        restore_pending_ = true;  // from here "saved_" is the only copy of the actors
    }
    next_generation_ = 2;
    const spp::ReloadResult loaded = loader_.load(std::move(*fresh.module));  // (e)
    if (!loaded) {
        result.error = "full reload failed: " + loaded.error + "\n(the saved actors stay for the next reload)";
        return result;
    }
    world_.Restore(saved_, [this] { return make_actor(); });  // (f) the actors use the new scripts
    saved_.clear();
    restore_pending_ = false;
    result.status = spp::ReloadStatus::Applied;
    return result;
}

std::string Harness::command(const std::string& line)
{
    switch (parse_command(line)) {
    case Command::Empty: return "";
    case Command::Quit:
        quit_ = true;
        return "quit\n";
    case Command::Stats: return loader_.stats_text();
    case Command::Reload: {
        const spp::ReloadResult result = reload();
        if (!result) return "reload failed: " + result.error + "\n";
        if (result.status == spp::ReloadStatus::Applied) return "full reload done\n";
        return "reload validated; it commits at the end of the next frame\n";
    }
    case Command::Unknown: break;
    }
    return "unknown command: " + line + " (use reload, stats or quit)\n";
}

void Harness::frame(float dt, int wait_ms)
{
    if (watcher_.wait_changed(wait_ms)) {
        const spp::ReloadResult result = reload();
        if (!result) std::fprintf(stderr, "[harness] reload failed: %s\n", result.error.c_str());
    }
    world_.Tick(dt);
}

}  // namespace harness
