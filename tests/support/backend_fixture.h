// The fixture of the tests that use the backend which SPP_BACKEND selected.
// The suite BackendTests has tests in several files. One file instantiates it.
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "spp/backend.h"

class BackendTests : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override { backend = spp::make_default_backend(); }
    std::unique_ptr<spp::Backend> backend;
};
