// Shared helpers for the GoogleTest files.
#pragma once

#include <gtest/gtest.h>

#include <string>

#include "Actor.h"
#include "script_runtime.h"

namespace test_support {

constexpr float kDt = 1.0f / 60.0f;

// One loader, one world and one Player. The loader is the first member, so it
// is the last to be destroyed. The libraries stay loaded while the world deletes
// the actors. Call start() inside ASSERT_NO_FATAL_FAILURE.
struct Session {
    script::Loader loader;
    World world;
    script::Instance inst{};
    Actor* actor = nullptr;
    bool commit_failed = false;

    // Load the library, make the Player and add it to the world.
    void start(const char* path)
    {
        std::string error;
        ASSERT_TRUE(loader.load(path, error)) << error;
        const script::ClassEntry* entry = loader.find("Player");
        ASSERT_NE(entry, nullptr) << "class Player is not registered";
        inst = entry->create();
        actor = static_cast<Actor*>(inst.native);
        world.Add(actor);
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
