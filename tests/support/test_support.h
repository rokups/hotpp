// Shared helpers for the GoogleTest files.
#pragma once

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "Actor.h"
#include "spp/runtime.h"
#include "support/reload_assert.h"

namespace test_support {

constexpr float kDt = 1.0f / 60.0f;

// One loader, one world and one Player. The loader is the first member, so it
// is the last to be destroyed. The libraries stay loaded while the world deletes
// the actors. Call start() inside ASSERT_NO_FATAL_FAILURE.
// The module of a library that exists. A failure throws; GoogleTest reports it.
inline spp::Module library(const char* path)
{
    spp::LoadResult r = spp::module_from_library(path);
    if (!r) throw std::runtime_error(r.error);
    return std::move(*r.module);
}

struct Session {
    spp::Loader loader;
    World world;
    spp::Instance inst{};
    Actor* actor = nullptr;
    bool commit_failed = false;

    // Register the module, make an object of the class (default: Player) and add it to the world.
    void start(spp::Module module, const char* class_name = "Player")
    {
        std::string error;
        ASSERT_RELOAD_OK(loader.load(std::move(module)));
        const spp::ClassEntry* entry = loader.find(class_name);
        ASSERT_NE(entry, nullptr) << "class " << class_name << " is not registered";
        inst = entry->create();
        actor = static_cast<Actor*>(inst.native);
        world.Add(actor);
    }

    // Load the library at the path as a module, then start.
    void start(const char* path)
    {
        spp::LoadResult r = spp::module_from_library(path);
        ASSERT_TRUE(r) << r.error;
        start(std::move(*r.module));
    }

    // Commit a pending reload at the frame boundary. Then let the loader unload
    // the retired generations that may unload.
    void commit_at_frame_boundary()
    {
        world.SetFrameBoundaryHook([this] {
            if (loader.reload_pending()) {
                if (const spp::ReloadResult res = loader.commit_pending(); !res) {
                    ADD_FAILURE() << "commit failed: " << res.error;
                    commit_failed = true;
                }
            }
            loader.frame_boundary();
        });
    }

    float* field(const char* name) { return spp::field_ptr<float>(inst.shell, name); }
};

}  // namespace test_support
