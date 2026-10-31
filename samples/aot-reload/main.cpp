// The program of the sample. The scripts are AOT code in the binary, and the program also has a
// backend. It runs three phases with one live object:
//   1. AOT: Loader::load_aot starts with the compiled-in script wolf.
//   2. override: the backend compiles script/wolf_override at run time. It replaces the unit wolf.
//   3. revert: Loader::revert_unit gives the unit wolf back to the compiled-in code.
// The object keeps its field values in all phases. The override needs a C++ compiler at run time.
// The build makes two programs from this file. The arguments select the script variant:
//   sample_aot_reload [script-dir [extension]]
// The folder defaults to the definition HOT_SCRIPT_DIR. The extension (.hot or .cpp) defaults to
// the definition HOT_SAMPLE_EXT, else .hot.
#include "Creature.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/runtime.h"
#include "hot/runtime/loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace
{
bool read_file(const std::string& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

std::string script_ext()
{
#ifdef HOT_SAMPLE_EXT
    return HOT_SAMPLE_EXT;
#else
    return ".hot";
#endif
}

// The folder of the scripts: argv[1], else the build definition HOT_SCRIPT_DIR. Empty if none.
std::string script_dir(int argc, char** argv)
{
    if (argc > 1)
        return argv[1];
#ifdef HOT_SCRIPT_DIR
    return HOT_SCRIPT_DIR;
#else
    return {};
#endif
}

int fail(const char* what, const std::string& detail = {})
{
    std::fprintf(stderr, "sample_aot_reload: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    return 1;
}

// The unit name of an AOT script is the name of the script file without the extension.
const char* const kUnit = "wolf";
} // namespace

int main(int argc, char** argv)
{
    const std::string dir = script_dir(argc, argv);
    if (dir.empty())
    {
        std::fprintf(stderr, "usage: sample_aot_reload <script-dir> [extension]\n");
        return 2;
    }
    const std::string ext = argc > 2 ? argv[2] : script_ext();
    const std::string override_path = dir + "/wolf_override" + ext;
    std::string override_text;
    if (!read_file(override_path, override_text))
        return fail("cannot read the override script", override_path);

    // The backend runs the compiler. The folder of Creature.h is an include dir of the compile.
    // The compile must use the same includes and flags as the AOT build of the scripts.
    hot::DefaultBackendOptions options;
    options.include_dirs = {std::filesystem::path(dir).parent_path().string()};
    std::unique_ptr<hot::Backend> backend = hot::make_default_backend(options);

    // The loader is first, so it is destroyed last. The script object dies before it.
    hot::Loader loader;

    // Give the backend the slot numbers of the AOT build. The override then keeps the numbers of
    // the methods, and the compiled-in code and the override agree on them.
    backend->seed_slots(hot::aot_table.slots);

    // Phase 1, AOT. hot::aot_table comes from the library sample_aot_reload_<variant>_scripts.
    const hot::ReloadResult started = loader.load_aot(hot::aot_table);
    if (!started)
        return fail("load_aot failed", started.error);
    const hot::ClassEntry* entry = loader.find("Wolf");
    if (!entry)
        return fail("the class Wolf is not available");
    hot::Instance instance = entry->create();
    Creature* wolf = static_cast<Creature*>(instance.native);

    std::printf("== aot\n");
    for (int frame = 0; frame < 2; ++frame)
        wolf->Tick(1.0f / 60.0f);

    // Phase 2, override. Compile the source and name the unit that it replaces. The override has
    // a newer generation than the active one. The commit is at a safe point of the host.
    hot::LoadResult built = backend->load({override_path, override_text, {}}, hot::active_generation + 1);
    if (!built)
        return fail("compile failed", built.error);
    built.module->unit = kUnit;
    const hot::ReloadResult requested = loader.request_reload(std::move(*built.module));
    if (!requested)
        return fail("override failed", requested.error);
    const hot::ReloadResult committed = loader.commit_pending();
    if (!committed)
        return fail("commit of the override failed", committed.error);
    loader.frame_boundary();

    std::printf("== override\n");
    for (int frame = 0; frame < 2; ++frame)
        wolf->Tick(1.0f / 60.0f);
    // The object kept bites (2 from phase 1) and got the new field howls.
    const int* bites = hot::field_ptr<int>(instance.shell, "bites");
    const int* howls = hot::field_ptr<int>(instance.shell, "howls");
    if (!bites || *bites != 4 || !howls || *howls != 2)
        return fail("the override did not keep the state");

    // Phase 3, revert. The compiled-in code of the unit is live again. The field howls does not
    // exist in the AOT layout, so the object drops it. The field bites stays.
    const hot::ReloadResult reverted = loader.revert_unit(kUnit, *backend);
    if (!reverted)
        return fail("revert failed", reverted.error);
    const hot::ReloadResult committed_revert = loader.commit_pending();
    if (!committed_revert)
        return fail("commit of the revert failed", committed_revert.error);
    loader.frame_boundary();

    std::printf("== revert\n");
    for (int frame = 0; frame < 2; ++frame)
        wolf->Tick(1.0f / 60.0f);
    bites = hot::field_ptr<int>(instance.shell, "bites");
    if (!bites || *bites != 6 || hot::field_ptr<int>(instance.shell, "howls"))
        return fail("the revert did not keep the state");

    std::printf("aot-reload: ok\n");
    std::fflush(stdout);
    delete wolf;
    return 0;
}
