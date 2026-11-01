// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The fixture of the tests that use the backend which HOT_BACKEND selected.
// The suite BackendTests has tests in several files. One file instantiates it.
#pragma once

#include "hot/backend.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

class BackendTests : public ::testing::TestWithParam<std::string>
{
protected:
    void SetUp() override { backend = hot::make_default_backend(); }
    std::unique_ptr<hot::Backend> backend;
};
