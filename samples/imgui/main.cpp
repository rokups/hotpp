// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// sample_imgui: a Dear ImGui window whose user interface a script draws.
// Usage: sample_imgui [script]
// The default script is samples/imgui/script/demo.hot or demo.cpp. The script class Demo
// derives from UiApp. The host calls Frame once for each frame.
// The struct Host loads the script and reloads it when the file changes.
#include "UiApp.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "hot/clang_files_backend.h"
#include "hot/paths.h"
#include "hot/runtime.h"
#include "hot/runtime/process.h"
#include "hot/scratch.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// The include folder of ImGui: the build definition (development build), else the environment.
std::string imgui_include_dir()
{
#ifdef HOT_IMGUI_INCLUDE_DIR
    return HOT_IMGUI_INCLUDE_DIR;
#else
    const char* env = std::getenv("HOT_IMGUI_INCLUDE_DIR");
    return env ? env : "";
#endif
}

// The default script: the definition HOT_SCRIPT_DIR with the extension of the variant.
std::string default_script()
{
#ifdef HOT_SCRIPT_DIR
    return std::string(HOT_SCRIPT_DIR) + "/demo" + HOT_SAMPLE_EXT;
#else
    return {};
#endif
}

// The folder for the work files and the ini file.
std::string app_work_dir()
{
    return hot::default_paths().work_dir;
}

std::unique_ptr<hot::Backend> make_backend(const std::string& script_path)
{
    const hot::DefaultPaths paths = hot::default_paths();
    hot::ClangFilesOptions o;
    o.compiler = paths.compiler;
    o.include_dirs = paths.include_dirs;
    // The folder of UiApp.h is the parent of the script folder.
    o.include_dirs.push_back(std::filesystem::path(script_path).parent_path().parent_path().string());
    if (const std::string imgui = imgui_include_dir(); !imgui.empty())
        o.include_dirs.push_back(imgui);
    o.extra_flags = paths.cxxflags;
    o.resource_dir = paths.resource_dir;
    const std::string work = app_work_dir();
    o.work_dir = work + "/p" + std::to_string(hot::detail::process_id());
    std::error_code ec;
    std::filesystem::remove_all(o.work_dir, ec);
    std::filesystem::create_directories(work, ec);
    // Only the p<pid> directory goes. The ini file in the work directory stays.
    o.remove_work_dir_at_exit = !hot::keep_temp();
    return std::make_unique<hot::ClangFilesBackend>(std::move(o));
}

bool read_file(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

// Reads and compiles the script. Returns false and sets "error" on failure.
bool compile_script(
    hot::Backend& backend, const std::string& path, uint32_t generation, hot::Module& out, std::string& error)
{
    std::string text;
    if (!read_file(path, text))
    {
        error = "cannot read " + path;
        return false;
    }
    hot::LoadResult r = backend.load({path, text}, generation);
    if (!r)
    {
        error = r.error;
        return false;
    }
    out = std::move(*r.module);
    return true;
}

// Makes the object of class Demo. Returns null and sets "error" on failure.
UiApp* create_app(hot::Loader& loader, std::string& error)
{
    const hot::ClassEntry* entry = loader.find("Demo");
    if (!entry)
    {
        error = "class Demo is not registered";
        return nullptr;
    }
    return static_cast<UiApp*>(entry->create().native);
}

// The host state of one run. The loader is the first member: it ends last.
struct Host
{
    hot::Loader loader;
    std::unique_ptr<hot::Backend> backend; // made in main, when the script path is known
    std::string path;
    uint32_t generation = 0; // the number of the last compile
    bool loaded = false; // the loader has a first module
    UiApp* app = nullptr;
    std::string last_error; // shown in the "Script error" window
    std::filesystem::file_time_type last_seen{};

    // The first load. It sets last_error on failure.
    void first_load()
    {
        std::error_code ec;
        last_seen = std::filesystem::last_write_time(path, ec);
        hot::Module m;
        if (!compile_script(*backend, path, ++generation, m, last_error))
        {
            std::fprintf(stderr, "load failed: %s\n", last_error.c_str());
            return;
        }
        loaded = true;
        if (const hot::ReloadResult res = loader.load(std::move(m)); !res)
        {
            last_error = res.error;
            loaded = false;
            std::fprintf(stderr, "load failed: %s\n", last_error.c_str());
            return;
        }
        app = create_app(loader, last_error);
        if (app)
            std::fprintf(stderr, "loaded gen %u\n", generation);
    }

    // Call it once for each frame, before the script runs. It starts a reload when
    // the file changed. The commit happens later, in frame_end.
    void poll_reload()
    {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(path, ec);
        if (ec || t == last_seen)
            return;
        last_seen = t;
        hot::Module m;
        std::string err;
        if (!compile_script(*backend, path, ++generation, m, err))
        {
            last_error = err;
            std::fprintf(stderr, "reload failed: %s\n", err.c_str());
            return;
        }
        if (!loaded)
        { // the first load failed: this module is the first one
            if (const hot::ReloadResult res = loader.load(std::move(m)); !res)
            {
                last_error = res.error;
                std::fprintf(stderr, "reload failed: %s\n", res.error.c_str());
                return;
            }
            loaded = true;
            recreate_app();
            return;
        }
        if (const hot::ReloadResult res = loader.request_reload(std::move(m)); !res)
        {
            last_error = res.error;
            std::fprintf(stderr, "reload failed: %s\n", res.error.c_str());
        }
        // Success: the error text stays until the commit works.
    }

    // Call it after app->Frame() and before Render. It is the frame boundary.
    void frame_end()
    {
        if (loader.reload_pending())
        {
            const hot::ReloadResult res = loader.commit_pending();
            if (res)
            {
                last_error.clear();
                std::fprintf(stderr, "reloaded gen %u\n", generation);
                if (!app)
                    recreate_app();
            }
            else
            {
                last_error = res.error;
                std::fprintf(stderr, "reload failed: %s\n", res.error.c_str());
            }
        }
        loader.frame_boundary();
    }

    void recreate_app()
    {
        std::string err;
        app = create_app(loader, err);
        if (app)
        {
            last_error.clear();
            std::fprintf(stderr, "reloaded gen %u (new object)\n", generation);
        }
        else
        {
            last_error = err;
        }
    }

    // Calls Frame. A throw drops the object until a good reload.
    void run_frame()
    {
        if (!app)
            return;
        try
        {
            app->Frame();
        }
        catch (const std::exception& e)
        {
            last_error = e.what();
            std::fprintf(stderr, "script error: %s\n", e.what());
            delete app;
            app = nullptr;
        }
    }
};

} // namespace

int main(int argc, char** argv)
{
    Host host; // the host ends last: the app is deleted before the loader
    host.path = argc > 1 ? argv[1] : default_script();
    if (host.path.empty())
    {
        std::fprintf(stderr, "usage: sample_imgui <script>\n");
        return 2;
    }
    host.backend = make_backend(host.path);
    host.first_load();

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        delete host.app;
        return 1;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("sample_imgui", 800, 600, SDL_WINDOW_RESIZABLE, &window, &renderer))
    {
        std::fprintf(stderr, "SDL window failed: %s\n", SDL_GetError());
        delete host.app;
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // The ini file goes to the app work directory. The backend does not remove it.
    static const std::string ini_path = app_work_dir() + "/imgui.ini";
    ImGui::GetIO().IniFilename = ini_path.c_str();
    // A script that throws inside Begin leaves the window stack open. ImGui recovers.
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }
        host.poll_reload();
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        host.run_frame();
        host.frame_end();
        if (!host.last_error.empty())
        {
            ImGui::Begin("Script error");
            ImGui::TextWrapped("%s", host.last_error.c_str());
            ImGui::End();
        }

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 36, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    delete host.app; // before the loader ends: the destructor code lives in the module
    host.app = nullptr;
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
