// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Move-only fields across a reload. The migration moves such a field into the new state
// block. A convert hook takes the old value by a move. The new object owns the same heap
// objects. The old block keeps an empty value, so its destruction frees nothing (an ASan build
// finds a double free or a leak).

#include "hot/backend.h"
#include "support/backend_fixture.h"
#include "support/reload_assert.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace
{

using test_support::kDt;
using test_support::Session;
using test_support::source;

using Owned = std::unique_ptr<int>;
using OwnedList = std::vector<std::unique_ptr<int>>;

template <class T> T* fld(hot::ShellCore* shell, const char* name)
{
    return hot::field_ptr<T>(shell, name);
}

// Loads generation 1 and generation 2 of "Holder" and checks that the reload moves the fields.
void check_move_only_reload(hot::Backend& backend, const char* v1_file, const char* v2_file)
{
    hot::LoadResult v1 = backend.load(source(v1_file), 1);
    ASSERT_TRUE(v1) << v1.error;
    hot::LoadResult v2 = backend.load(source(v2_file), 2);
    ASSERT_TRUE(v2) << v2.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.actor->Tick(kDt);
    s.actor->Tick(kDt);

    Owned* one = fld<Owned>(s.inst.shell, "one");
    OwnedList* many = fld<OwnedList>(s.inst.shell, "many");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(many, nullptr);
    ASSERT_NE(one->get(), nullptr);
    EXPECT_EQ(**one, 7);
    ASSERT_EQ(many->size(), 2u);
    const int* one_before = one->get();
    const int* first_before = (*many)[0].get();
    ASSERT_NE(fld<Owned>(s.inst.shell, "level"), nullptr);
    const int* level_before = fld<Owned>(s.inst.shell, "level")->get();

    ASSERT_RELOAD_OK(s.loader.request_reload(std::move(*v2.module)));
    const hot::ReloadResult res = s.loader.commit_pending();
    ASSERT_TRUE(res) << res.error;
    EXPECT_EQ(hot::active_generation, 2u);

    // The same heap objects: the migration moved the fields. The initializer did not run.
    one = fld<Owned>(s.inst.shell, "one");
    many = fld<OwnedList>(s.inst.shell, "many");
    ASSERT_NE(one, nullptr);
    ASSERT_NE(many, nullptr);
    EXPECT_EQ(one->get(), one_before);
    EXPECT_EQ(**one, 7);
    ASSERT_EQ(many->size(), 2u);
    EXPECT_EQ((*many)[0].get(), first_before);
    EXPECT_EQ(*(*many)[1], 3);
    ASSERT_NE(fld<int>(s.inst.shell, "ticks"), nullptr);
    EXPECT_EQ(*fld<int>(s.inst.shell, "ticks"), 0); // a new field takes its initializer
    // The convert hook got the old unique_ptr by a move: the shared_ptr owns the same int.
    ASSERT_EQ(fld<Owned>(s.inst.shell, "level"), nullptr);
    std::shared_ptr<int>* level = fld<std::shared_ptr<int>>(s.inst.shell, "level");
    ASSERT_NE(level, nullptr);
    EXPECT_EQ(level->get(), level_before);
    EXPECT_EQ(**level, 2);

    s.actor->Tick(kDt); // the Tick of generation 2
    EXPECT_EQ(*fld<int>(s.inst.shell, "ticks"), 1);
    ASSERT_EQ(many->size(), 3u);
    EXPECT_EQ(*(*many)[2], 4);
}

TEST_P(BackendTests, MoveOnlyFieldsOfAHotClassMoveAcrossAReload)
{
    check_move_only_reload(*backend, "move_only_v1.hot", "move_only_v2.hot");
}

TEST_P(BackendTests, MoveOnlyFieldsOfACppClassMoveAcrossAReload)
{
    check_move_only_reload(*backend, "cpp_move_only_v1.cpp", "cpp_move_only_v2.cpp");
}

} // namespace
