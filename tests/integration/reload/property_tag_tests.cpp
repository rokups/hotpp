// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A property with a script attribute tag has a row in the property table of the layout. The
// thunks of the row call the accessors. The tests use the backend that HOT_BACKEND selected.
#include "hot/backend.h"
#include "hot/runtime.h"
#include "support/backend_fixture.h"
#include "support/script_files.h"
#include "support/test_support.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace
{

using test_support::Session;
using test_support::source;

const hot::PropertyInfo* find_property(const hot::StateLayout& layout, const char* name)
{
    for (uint32_t i = 0; i < layout.prop_count; ++i)
        if (std::strcmp(layout.props[i].name, name) == 0)
            return &layout.props[i];
    return nullptr;
}

TEST_P(BackendTests, PropertyTagRowsReadAndWriteThroughTheAccessors)
{
    hot::LoadResult v1 = backend->load(source("property_tag_v1.hot"), 1);
    ASSERT_TRUE(v1) << v1.error;
    Session s;
    ASSERT_NO_FATAL_FAILURE(s.start(std::move(*v1.module), "Holder"));
    s.commit_at_frame_boundary();

    auto* shell = static_cast<hot::ShellBase*>(s.inst.shell);
    const hot::StateLayout* layout = s.inst.shell->cls->layout;
    ASSERT_NE(layout, nullptr);
    EXPECT_EQ(layout->prop_count, 2u); // Health and Speed, not Plain
    const hot::PropertyInfo* health = find_property(*layout, "Health");
    const hot::PropertyInfo* speed = find_property(*layout, "Speed");
    ASSERT_NE(health, nullptr);
    ASSERT_NE(speed, nullptr);
    EXPECT_EQ(find_property(*layout, "Plain"), nullptr);
    ASSERT_NE(health->attributes, nullptr);
    EXPECT_STREQ(health->attributes[0].name, "Attribute");
    EXPECT_STREQ(health->attributes[0].args[0], "Max Health");
    EXPECT_STREQ(health->attributes[0].args[1], "AM_DEFAULT");
    EXPECT_TRUE(hot::type_key_matches(health->type_key, hot::type_key<int32_t>()));
    EXPECT_TRUE(hot::type_key_matches(speed->type_key, hot::type_key<float>()));

    int32_t hp = 0;
    health->get(shell, &hp);
    EXPECT_EQ(hp, 40);
    const int32_t next = 21;
    health->set(shell, &next); // the setter doubles the value
    health->get(shell, &hp);
    EXPECT_EQ(hp, 42);
    float v = 0;
    speed->get(shell, &v);
    EXPECT_EQ(v, 1.5f);
    const float faster = 3.0f;
    speed->set(shell, &faster);
    speed->get(shell, &v);
    EXPECT_EQ(v, 3.0f);
}

} // namespace
