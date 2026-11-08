// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A host that does not use hot::Task in its own C++ code. This file builds as C++17, so
// task.h is empty here. A script with async methods must load all the same: the runtime
// must export the Task helpers also when the host does not pull them in.
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/script_files.h"
#include <gtest/gtest.h>
#include <string>

#if defined(__cpp_impl_coroutine)
#error "this host must not use C++20 coroutines"
#endif

using test_support::source;

namespace
{

void expect_async_script_loads(hot::BackendKind kind)
{
    if (!hot::backend_available(kind))
        GTEST_SKIP() << "the backend is not available in this build";
    hot::DefaultBackendOptions o;
    o.kind = kind;
    o.std_version = "c++20";
    o.include_dirs = {std::string(HOT_SOURCE_DIR) + "/tests/engine"};
    auto backend = hot::make_default_backend(o);
    hot::LoadResult built = backend->load(source("async_override.hot"), 1);
    ASSERT_TRUE(built) << built.error;
    hot::Loader loader;
    const hot::ReloadResult r = loader.load(std::move(*built.module));
    ASSERT_TRUE(r) << r.error;
    EXPECT_NE(loader.find("AsyncDerived"), nullptr);
}

} // namespace

TEST(PlainHost, AClangFilesScriptWithAsyncMethodsLoads)
{
    expect_async_script_loads(hot::BackendKind::ClangFiles);
}

TEST(PlainHost, AJitScriptWithAsyncMethodsLoads)
{
    expect_async_script_loads(hot::BackendKind::Jit);
}
