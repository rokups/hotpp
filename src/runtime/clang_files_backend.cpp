#include "spp/clang_files_backend.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "spp/spc_lib.h"

extern char** environ;

namespace script {

namespace fs = std::filesystem;

int run_command(const std::vector<std::string>& argv, std::string& output)
{
    output.clear();
    if (argv.empty()) return -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    std::vector<char*> cargv;
    for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        output = "cannot run '" + argv[0] + "': " + std::strerror(rc) + "\n";
        return -1;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0) output.append(buf, static_cast<size_t>(n));
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

namespace {

bool read_text(const std::string& path, std::string& text)
{
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

bool write_text(const std::string& path, const std::string& text)
{
    std::ofstream f(path);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

}  // namespace

ClangFilesBackend::ClangFilesBackend(ClangFilesOptions options) : opt_(std::move(options))
{
    if (!opt_.session) opt_.session = std::make_shared<spc::TranslatorSession>();
    if (opt_.slots_file.empty()) opt_.slots_file = opt_.work_dir + "/slots.txt";
}

bool ClangFilesBackend::build(const ScriptSource& source, uint32_t generation,
                              const std::string& out_so, std::string& error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    std::error_code ec;
    const fs::path so_path(out_so);
    if (so_path.has_parent_path()) fs::create_directories(so_path.parent_path(), ec);
    fs::create_directories(fs::path(opt_.slots_file).parent_path().empty()
                               ? fs::path(".")
                               : fs::path(opt_.slots_file).parent_path(),
                           ec);
    const std::string cpp_path = fs::path(so_path).replace_extension(".cpp").string();

    spc::TranslateOptions to;
    to.script_text = source.text;
    to.script_name = source.name;
    to.output_name = cpp_path;
    to.generation = static_cast<int>(generation);
    to.include_dirs = opt_.include_dirs;
    to.resource_dir = opt_.resource_dir;
    read_text(opt_.slots_file, to.slots_in);  // a missing file means the first generation
    spc::TranslateResult tr = spc::translate(to, *opt_.session);
    if (!tr.ok) {
        error = tr.diagnostics;
        return false;
    }
    if (!write_text(cpp_path, tr.cpp)) {
        error = "cannot write " + cpp_path;
        return false;
    }

    std::vector<std::string> cmd = {opt_.compiler, "-std=c++20", "-fPIC", "-shared", "-g"};
    if (!opt_.resource_dir.empty()) cmd.push_back("-resource-dir=" + opt_.resource_dir);
    for (const std::string& f : opt_.extra_flags) cmd.push_back(f);
    for (const std::string& d : opt_.include_dirs) cmd.push_back("-I" + d);
    cmd.push_back(cpp_path);
    cmd.push_back("-o");
    cmd.push_back(out_so);
    fs::remove(so_path, ec);
    std::string output;
    const int rc = run_command(cmd, output);
    if (rc != 0) {
        error = output + "the compile of " + source.name + " failed (exit code " +
                std::to_string(rc) + ")\n";
        return false;
    }
    if (!write_text(opt_.slots_file, tr.slots_out)) {
        error = "cannot write " + opt_.slots_file;
        return false;
    }
    return true;
}

LoadResult ClangFilesBackend::load(const ScriptSource& source, uint32_t generation)
{
    std::string stem = fs::path(source.name).stem().string();
    if (stem.empty()) stem = "script";
    const std::string so = opt_.work_dir + "/" + stem + ".g" + std::to_string(generation) + "." +
                           std::to_string(sequence_++) + ".so";
    LoadResult r;
    if (!build(source, generation, so, r.error)) return r;
    return module_from_library(so);
}

}  // namespace script
