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
    if (!loader_.load(std::move(*r.module), error)) return false;
    const spp::ClassEntry* entry = loader_.find(class_name_);
    if (!entry) {
        error = "class " + class_name_ + " is not registered";
        return false;
    }
    world_.Add(static_cast<Actor*>(entry->create().native));
    world_.SetFrameBoundaryHook([this] {
        std::string err;
        if (loader_.reload_pending() && !loader_.commit_pending(err) && !err.empty())
            std::fprintf(stderr, "[harness] commit failed: %s\n", err.c_str());
        loader_.frame_boundary();
    });
    return true;
}

bool Harness::reload(std::string& error)
{
    ++reloads_;
    spp::LoadResult r = backend_->load(read_source(script_), next_generation_++);
    if (!r) {
        error = r.error;
        return false;
    }
    return loader_.request_reload(std::move(*r.module), error);
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
        std::string error;
        if (!reload(error)) return "reload failed: " + error + "\n";
        return "reload validated; it commits at the end of the next frame\n";
    }
    case Command::Unknown: break;
    }
    return "unknown command: " + line + " (use reload, stats or quit)\n";
}

void Harness::frame(float dt, int wait_ms)
{
    if (watcher_.wait_changed(wait_ms)) {
        std::string error;
        if (!reload(error)) std::fprintf(stderr, "[harness] reload failed: %s\n", error.c_str());
    }
    world_.Tick(dt);
}

}  // namespace harness
