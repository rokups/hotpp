// Shared helpers for the GoogleTest files.
#pragma once

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "Actor.h"
#include "script_runtime.h"

namespace test_support {

constexpr float kDt = 1.0f / 60.0f;

// One loader, one world and one Player. The loader is the first member, so it
// is the last to be destroyed. The libraries stay loaded while the world deletes
// the actors. Call start() inside ASSERT_NO_FATAL_FAILURE.
// The module of a library that exists. A failure throws; GoogleTest reports it.
inline script::Module library(const char* path)
{
    script::LoadResult r = script::module_from_library(path);
    if (!r) throw std::runtime_error(r.error);
    return std::move(*r.module);
}

struct Session {
    script::Loader loader;
    World world;
    script::Instance inst{};
    Actor* actor = nullptr;
    bool commit_failed = false;

    // Register the module, make the Player and add it to the world.
    void start(script::Module module)
    {
        std::string error;
        ASSERT_TRUE(loader.load(std::move(module), error)) << error;
        const script::ClassEntry* entry = loader.find("Player");
        ASSERT_NE(entry, nullptr) << "class Player is not registered";
        inst = entry->create();
        actor = static_cast<Actor*>(inst.native);
        world.Add(actor);
    }

    // Load the library at the path as a module, then start.
    void start(const char* path)
    {
        script::LoadResult r = script::module_from_library(path);
        ASSERT_TRUE(r) << r.error;
        start(std::move(*r.module));
    }

    // Commit a pending reload at the frame boundary.
    void commit_at_frame_boundary()
    {
        world.SetFrameBoundaryHook([this] {
            std::string err;
            if (loader.reload_pending() && !loader.commit_pending(err)) {
                ADD_FAILURE() << "commit failed: " << err;
                commit_failed = true;
            }
        });
    }

    float* field(const char* name) { return script::field_ptr<float>(inst.shell, name); }
};

}  // namespace test_support
