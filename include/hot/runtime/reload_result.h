// The result types of a reload: the status, the reason and the classes involved.
//
// module.h (LoadResult) and loader.h (Loader) both use them. So they live in a small header
// that includes nothing from the runtime.
#pragma once
#include <string>
#include <vector>

namespace hot
{

enum class ReloadStatus
{
    Applied, // the new generation is active
    Pending, // the loader accepted the module; commit_pending applies it
    FullReloadRequired, // hot reload cannot apply the change; the host must do a full reload
    Failed, // the module is not acceptable; the live generation continues
};

enum class ReloadReason
{
    None,
    NativeBaseChanged, // the native base class of a script class changed
    InterfaceBasesChanged, // a native interface base was added or removed
    ScriptParentChanged, // the script parent of a class changed
    ShellLayoutChanged, // the layout of the shell of a class changed
    SlotsRemoved, // a class has fewer slots in the new generation
    CompileError, // the backend could not build the module
    BadModule, // the module has no register function, no layout or no slot
    GenerationNotNewer, // the generation of a class is not newer than the active one
    Busy, // a reload is already pending, or the commit cannot run now
    MigrationFailed, // the commit could not migrate the state
};

// The name of a status or a reason, for a log or a test message.
inline const char* to_string(ReloadStatus status)
{
    switch (status)
    {
    case ReloadStatus::Applied: return "Applied";
    case ReloadStatus::Pending: return "Pending";
    case ReloadStatus::FullReloadRequired: return "FullReloadRequired";
    case ReloadStatus::Failed: return "Failed";
    }
    return "?";
}

inline const char* to_string(ReloadReason reason)
{
    switch (reason)
    {
    case ReloadReason::None: return "None";
    case ReloadReason::NativeBaseChanged: return "NativeBaseChanged";
    case ReloadReason::InterfaceBasesChanged: return "InterfaceBasesChanged";
    case ReloadReason::ScriptParentChanged: return "ScriptParentChanged";
    case ReloadReason::ShellLayoutChanged: return "ShellLayoutChanged";
    case ReloadReason::SlotsRemoved: return "SlotsRemoved";
    case ReloadReason::CompileError: return "CompileError";
    case ReloadReason::BadModule: return "BadModule";
    case ReloadReason::GenerationNotNewer: return "GenerationNotNewer";
    case ReloadReason::Busy: return "Busy";
    case ReloadReason::MigrationFailed: return "MigrationFailed";
    }
    return "?";
}

struct ReloadResult
{
    ReloadStatus status = ReloadStatus::Failed;
    ReloadReason reason = ReloadReason::None;
    std::string error; // the text for a log; empty on success
    std::vector<std::string> classes; // the names of the classes involved

    // True for Applied and Pending.
    explicit operator bool() const { return status == ReloadStatus::Applied || status == ReloadStatus::Pending; }
};

} // namespace hot
