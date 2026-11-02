// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Tests of hot::abstract_called and of the is_abstract flag of ClassRecord.
#include "hot/runtime/errors.h"
#include "hot/runtime/records.h"

#include <gtest/gtest.h>

#include <string>

TEST(AbstractCalled, ThrowsScriptErrorWithClassAndFunction)
{
    try
    {
        hot::abstract_called("Shape", "Area");
        FAIL() << "abstract_called returned";
    }
    catch (const hot::ScriptError& e)
    {
        EXPECT_EQ(std::string(e.what()), "abstract method 'Shape.Area' called");
    }
}

TEST(AbstractCalled, TheExceptionIsAScriptError)
{
    EXPECT_THROW(hot::abstract_called("C", "F"), hot::ScriptError);
}

TEST(AbstractCalled, RecordIsNotAbstractByDefault)
{
    hot::ClassRecord rec{};
    EXPECT_FALSE(rec.is_abstract);
    rec.is_abstract = true;
    EXPECT_TRUE(rec.is_abstract);
}
