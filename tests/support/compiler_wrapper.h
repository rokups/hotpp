// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A compiler wrapper for the tests of the parallel build. It is a POSIX shell script. It passes all
// arguments to the real compiler. A "-c" call writes a line to the file "calls" and makes the marker
// "running.<pid>" in the test directory. When two markers exist, it makes the file "overlap". It
// ends its marker at the end (the script renames it to "done.<pid>"). This proves that two compiles ran at the same time.
//
// Wait mode: a "-c" call waits for the file "overlap" (at most the wait time, in seconds). With a
// match text, only the "-c" calls that have this text in an argument wait.
// Hold mode: a matching "-c" call sleeps for the hold time (a decimal number of seconds) after it
// starts. This makes the overlaps visible. Each "-c" call appends the count of the markers to the
// file "counts". The function peak() gives the highest count.
// The script reads the directory, the wait time and the match text from the environment.
#pragma once

#include "support/test_env.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace test_support
{

// Sets an environment variable for the life of the object. The destructor restores the old value.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, const char* value)
        : name_(name)
    {
        if (const char* old = std::getenv(name))
        {
            had_ = true;
            old_ = old;
        }
        set_env(name, value);
    }
    ~ScopedEnv()
    {
        if (had_)
            set_env(name_.c_str(), old_.c_str());
        else
            unset_env(name_.c_str());
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::string old_;
    bool had_ = false;
};

class CompilerWrapper
{
public:
    // "dir" is a new test directory. "real_compiler" is the compiler that the script calls.
    CompilerWrapper(const std::filesystem::path& dir, const std::string& real_compiler)
        : dir_(dir)
        , dir_env_("HOT_TEST_WRAP_DIR", "")
        , wait_env_("HOT_TEST_WRAP_WAIT", "")
        , match_env_("HOT_TEST_WRAP_MATCH", "")
        , hold_env_("HOT_TEST_WRAP_HOLD", "")
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_);
        script_ = (dir_ / "cxx_wrapper.sh").string();
        std::ofstream(script_, std::ios::binary)
            << "#!/bin/sh\n"
               "dir=\"$HOT_TEST_WRAP_DIR\"\n"
               "compile=0\n"
               "match=0\n"
               "for a in \"$@\"; do\n"
               "  [ \"$a\" = \"-c\" ] && compile=1\n"
               "  case \"$a\" in *\"$HOT_TEST_WRAP_MATCH\"*) match=1;; esac\n"
               "done\n"
               "if [ $compile = 1 ]; then\n"
               "  echo \"$$\" >> \"$dir/calls\"\n"
               "  touch \"$dir/running.$$\"\n"
               "  n=$(ls \"$dir\" | grep -c '^running\\.')\n"
               "  echo \"$n\" >> \"$dir/counts\"\n"
               "  [ \"$n\" -ge 2 ] && touch \"$dir/overlap\"\n"
               "  if [ -n \"$HOT_TEST_WRAP_HOLD\" ] && [ $match = 1 ]; then\n"
               "    sleep \"$HOT_TEST_WRAP_HOLD\"\n"
               "  fi\n"
               "  if [ -n \"$HOT_TEST_WRAP_WAIT\" ] && [ \"$HOT_TEST_WRAP_WAIT\" -gt 0 ] && [ $match = 1 ]; then\n"
               "    i=0\n"
               "    while [ ! -e \"$dir/overlap\" ] && [ $i -lt $((HOT_TEST_WRAP_WAIT * 20)) ]; do\n"
               "      sleep 0.05\n"
               "      i=$((i + 1))\n"
               "    done\n"
               "  fi\n"
               "fi\n"
               "\"" + real_compiler + "\" \"$@\"\n"
               "rc=$?\n"
               "[ $compile = 1 ] && mv -f \"$dir/running.$$\" \"$dir/done.$$\"\n"
               "exit $rc\n";
        std::filesystem::permissions(script_,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec,
            std::filesystem::perm_options::replace);
        set_env("HOT_TEST_WRAP_DIR", dir_.string().c_str());
    }

    // The path of the script. Use it as the compiler.
    const std::string& path() const { return script_; }
    // Wait mode on: a matching "-c" call waits at most "seconds" for the file "overlap". 0: off.
    // "match" is a text of an argument (empty: all "-c" calls wait).
    void set_wait(unsigned seconds, const std::string& match = "")
    {
        set_env("HOT_TEST_WRAP_WAIT", std::to_string(seconds).c_str());
        set_env("HOT_TEST_WRAP_MATCH", match.c_str());
    }
    // Hold mode: a matching "-c" call sleeps "seconds" (for example "0.3"). Empty: off.
    void set_hold(const std::string& seconds, const std::string& match = "")
    {
        set_env("HOT_TEST_WRAP_HOLD", seconds.c_str());
        set_env("HOT_TEST_WRAP_MATCH", match.c_str());
    }
    // The highest number of "-c" calls that ran at the same time since the start or the last reset.
    size_t peak() const
    {
        std::ifstream f(dir_ / "counts");
        size_t best = 0;
        for (size_t n = 0; f >> n;)
            best = std::max(best, n);
        return best;
    }
    // True when two "-c" calls ran at the same time.
    bool overlap() const { return std::filesystem::exists(dir_ / "overlap"); }
    // The number of the "-c" calls since the start or the last reset.
    size_t compile_calls() const
    {
        std::ifstream f(dir_ / "calls");
        size_t n = 0;
        for (std::string line; std::getline(f, line);)
            ++n;
        return n;
    }
    // Clears the call log and the overlap flag.
    void reset()
    {
        std::error_code ec;
        std::filesystem::remove(dir_ / "calls", ec);
        std::filesystem::remove(dir_ / "overlap", ec);
        std::filesystem::remove(dir_ / "counts", ec);
    }

private:
    std::filesystem::path dir_;
    std::string script_;
    ScopedEnv dir_env_;
    ScopedEnv wait_env_;
    ScopedEnv match_env_;
    ScopedEnv hold_env_;
};

} // namespace test_support
