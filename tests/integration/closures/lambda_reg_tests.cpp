// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A lambda in a method that never runs registers its record at module load (spike). The tests run
// with the clang files backend, with the JIT backend and with the AOT code of the script. A mode
// that is not in the build is skipped.
//
// CMake defines HOT_SOURCE_DIR.
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/script_files.h"

#include <gtest/gtest.h>

#include <typeinfo>

namespace
{

enum class Mode
{
    Files,
    Jit,
    Aot,
};

class LambdaReg : public ::testing::TestWithParam<Mode>
{
};

TEST_P(LambdaReg, UnusedLambdaIsRegisteredAtLoad)
{
    const Mode m = GetParam();
    if (m == Mode::Files && !hot::backend_available(hot::BackendKind::ClangFiles))
        GTEST_SKIP() << "the clang files backend is not in this build";
    if (m == Mode::Jit && !hot::backend_available(hot::BackendKind::Jit))
        GTEST_SKIP() << "the JIT backend is not in this build";
    hot::Loader loader;
    std::unique_ptr<hot::Backend> backend;
    if (m == Mode::Aot)
    {
        const hot::ReloadResult r = loader.load_aot(hot::aot_table);
        ASSERT_TRUE(r) << r.error;
    }
    else
    {
        hot::DefaultBackendOptions o;
        o.kind = m == Mode::Files ? hot::BackendKind::ClangFiles : hot::BackendKind::Jit;
        o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine"};
        backend = hot::make_default_backend(o);
        hot::LoadResult built = backend->load(test_support::source("lambda_reg.cpp"), 1);
        ASSERT_TRUE(built) << built.error;
        const hot::ReloadResult r = loader.load(std::move(*built.module));
        ASSERT_TRUE(r) << r.error;
    }
    const hot::LambdaRecord* rec = loader.find_lambda("LambdaHolder::Never#0");
    ASSERT_NE(rec, nullptr);
    EXPECT_STREQ(rec->key, "LambdaHolder::Never#0");
    EXPECT_STREQ(rec->sig, typeid(int(int)).name());
    EXPECT_NE(rec->entry, nullptr);
}

const char* mode_name(const ::testing::TestParamInfo<Mode>& info)
{
    return info.param == Mode::Files ? "Files" : info.param == Mode::Jit ? "Jit" : "Aot";
}

INSTANTIATE_TEST_SUITE_P(Backends, LambdaReg, ::testing::Values(Mode::Files, Mode::Jit, Mode::Aot), mode_name);

} // namespace
