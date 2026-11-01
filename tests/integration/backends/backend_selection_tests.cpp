// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of the run-time choice of the backend: backend_available, DefaultBackendOptions::kind
// and the environment variable HOT_BACKEND. CMake defines HOT_WITH_CLANG_FILES and HOT_WITH_JIT
// (on hot_backend) for the backends that the build has.
#include "hot/backend.h"
#include "support/test_env.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{

#ifdef HOT_WITH_CLANG_FILES
constexpr bool kHasClangFiles = true;
#else
constexpr bool kHasClangFiles = false;
#endif
#ifdef HOT_WITH_JIT
constexpr bool kHasJit = true;
#else
constexpr bool kHasJit = false;
#endif

// Sets or removes the environment variable HOT_BACKEND. The destructor restores the old value.
class ScopedBackendEnv
{
public:
    explicit ScopedBackendEnv(const char* value)
    {
        if (const char* old = std::getenv("HOT_BACKEND"))
            old_ = old;
        set(value);
    }
    ~ScopedBackendEnv() { set(old_ ? old_->c_str() : nullptr); }
    ScopedBackendEnv(const ScopedBackendEnv&) = delete;
    ScopedBackendEnv& operator=(const ScopedBackendEnv&) = delete;

private:
    static void set(const char* value)
    {
        if (value)
            test_support::set_env("HOT_BACKEND", value);
        else
            test_support::unset_env("HOT_BACKEND");
    }
    std::optional<std::string> old_;
};

std::string name_of(hot::BackendKind kind)
{
    hot::DefaultBackendOptions options;
    options.kind = kind;
    return hot::make_default_backend(options)->name();
}

// The message of the error that make_default_backend throws. Empty: no error.
std::string error_of(hot::BackendKind kind)
{
    try
    {
        name_of(kind);
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }
    return {};
}

TEST(BackendSelection, AvailableMatchesTheBuild)
{
    EXPECT_EQ(hot::backend_available(hot::BackendKind::ClangFiles), kHasClangFiles);
    EXPECT_EQ(hot::backend_available(hot::BackendKind::Jit), kHasJit);
    EXPECT_EQ(hot::backend_available(hot::BackendKind::Auto), kHasClangFiles || kHasJit);
}

TEST(BackendSelection, AutoWithoutVariableUsesClangFilesFirst)
{
    ScopedBackendEnv env(nullptr);
    EXPECT_EQ(name_of(hot::BackendKind::Auto), kHasClangFiles ? "CLANG_FILES" : "JIT");
    EXPECT_EQ(std::string(hot::make_default_backend()->name()), kHasClangFiles ? "CLANG_FILES" : "JIT");
}

TEST(BackendSelection, ExplicitKindSelectsTheBackend)
{
    ScopedBackendEnv env(nullptr);
    if (kHasClangFiles)
        EXPECT_EQ(name_of(hot::BackendKind::ClangFiles), "CLANG_FILES");
    if (kHasJit)
        EXPECT_EQ(name_of(hot::BackendKind::Jit), "JIT");
}

TEST(BackendSelection, VariableSelectsTheBackend)
{
    if (kHasJit)
    {
        ScopedBackendEnv env("jit");
        EXPECT_EQ(name_of(hot::BackendKind::Auto), "JIT");
        EXPECT_EQ(std::string(hot::make_default_backend()->name()), "JIT");
    }
    if (kHasClangFiles)
    {
        // "files" and its old spellings (any case) select the clang files backend.
        for (const char* value : {"files", "FILES", "clang_files", "CLANG_FILES", "clang-files"})
        {
            ScopedBackendEnv env(value);
            EXPECT_EQ(name_of(hot::BackendKind::Auto), "CLANG_FILES") << value;
        }
    }
}

TEST(BackendSelection, ExplicitKindWinsOverTheVariable)
{
    if (kHasClangFiles)
    {
        ScopedBackendEnv env("jit");
        EXPECT_EQ(name_of(hot::BackendKind::ClangFiles), "CLANG_FILES");
    }
    if (kHasJit)
    {
        ScopedBackendEnv env("files");
        EXPECT_EQ(name_of(hot::BackendKind::Jit), "JIT");
    }
}

TEST(BackendSelection, WrongVariableValueIsAnError)
{
    ScopedBackendEnv env("fast");
    const std::string error = error_of(hot::BackendKind::Auto);
    EXPECT_NE(error.find("HOT_BACKEND"), std::string::npos) << error;
    EXPECT_NE(error.find("\"jit\" or \"files\""), std::string::npos) << error;
}

#ifndef HOT_WITH_JIT
TEST(BackendSelection, DisabledJitIsAnError)
{
    ScopedBackendEnv env(nullptr);
    const std::string error = error_of(hot::BackendKind::Jit);
    EXPECT_NE(error.find("HOT_ENABLE_JIT"), std::string::npos) << error;
    ScopedBackendEnv jit("jit");
    EXPECT_NE(error_of(hot::BackendKind::Auto).find("HOT_ENABLE_JIT"), std::string::npos);
}
#endif

#ifndef HOT_WITH_CLANG_FILES
TEST(BackendSelection, DisabledClangFilesIsAnError)
{
    ScopedBackendEnv env(nullptr);
    const std::string error = error_of(hot::BackendKind::ClangFiles);
    EXPECT_NE(error.find("HOT_ENABLE_CLANG_FILES"), std::string::npos) << error;
    ScopedBackendEnv files("files");
    EXPECT_NE(error_of(hot::BackendKind::Auto).find("HOT_ENABLE_CLANG_FILES"), std::string::npos);
}
#endif

} // namespace
