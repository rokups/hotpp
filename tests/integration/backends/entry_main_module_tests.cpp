// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of Module::entry_main: the runtime finds "hot_main" when a backend loads a module.
// Each test runs for each backend that the build has.
#include "hot/backend.h"
#include "hot/module.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{

std::vector<hot::BackendKind> backend_kinds()
{
    std::vector<hot::BackendKind> kinds;
    for (hot::BackendKind kind : {hot::BackendKind::ClangFiles, hot::BackendKind::Jit})
        if (hot::backend_available(kind))
            kinds.push_back(kind);
    return kinds;
}

std::unique_ptr<hot::Backend> backend_of(hot::BackendKind kind)
{
    hot::DefaultBackendOptions o;
    o.kind = kind;
    return hot::make_default_backend(o);
}

TEST(EntryMainModule, ModuleWithMainHasEntryMain)
{
    for (hot::BackendKind kind : backend_kinds())
    {
        auto backend = backend_of(kind);
        SCOPED_TRACE(backend->name());
        const hot::ScriptSource src{"entry_args.hot", "int main(std.string[] args) { return args.size() * 10; }\n", {}};
        hot::LoadResult r = backend->load(src, 1);
        ASSERT_TRUE(r) << r.error;
        ASSERT_NE(r.module->entry_main, nullptr);
        const char* argv[] = {"prog", "a", "b"};
        EXPECT_EQ(r.module->entry_main(3, argv), 30);
        // The pointer survives a move of the Module.
        hot::Module moved = std::move(*r.module);
        ASSERT_NE(moved.entry_main, nullptr);
        EXPECT_EQ(moved.entry_main(2, argv), 20);
    }
}

TEST(EntryMainModule, VoidMainGivesZero)
{
    for (hot::BackendKind kind : backend_kinds())
    {
        auto backend = backend_of(kind);
        SCOPED_TRACE(backend->name());
        const hot::ScriptSource src{"entry_void.hot", "void main() { }\n", {}};
        hot::LoadResult r = backend->load(src, 1);
        ASSERT_TRUE(r) << r.error;
        ASSERT_NE(r.module->entry_main, nullptr);
        EXPECT_EQ(r.module->entry_main(0, nullptr), 0);
    }
}

TEST(EntryMainModule, ModuleWithoutMainHasNoEntryMain)
{
    for (hot::BackendKind kind : backend_kinds())
    {
        auto backend = backend_of(kind);
        SCOPED_TRACE(backend->name());
        const hot::ScriptSource src{"entry_none.hot", "int helper() { return 1; }\n", {}};
        hot::LoadResult r = backend->load(src, 1);
        ASSERT_TRUE(r) << r.error;
        EXPECT_EQ(r.module->entry_main, nullptr);
    }
}

} // namespace
