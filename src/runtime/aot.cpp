// The AOT runtime (design M7): the AOT registry and the object tracking of a
// program that has no loader. It does not include loader.h.
#include "hot/aot.h"

#include "hot/runtime/shell.h"
#include "hot/runtime/state.h"
#include "hot/runtime/threads.h"

#include <algorithm>
#include <stdexcept>

namespace hot
{

// An AOT program never reloads. Nobody reads the list of live objects, so the calls do nothing.
void track_object(ShellBase*)
{
}
void untrack_object(ShellBase*)
{
}
void note_creator(ShellBase*, uint32_t)
{
}

namespace
{

// The generation number of all AOT code. The guard of each generation function reads it.
constexpr uint32_t kAotGeneration = 1;

void destroy_module_block(const ModuleStateInfo& info)
{
    if (!info.slot || !*info.slot)
        return;
    char* block = static_cast<char*>(*info.slot);
    destroy_fields(info.layout, block);
    aligned_free(block);
    *info.slot = nullptr;
}

} // namespace

AotRegistry::AotRegistry(const AotTable& table)
{
    active_generation = kAotGeneration;
    try
    {
        for (size_t i = 0; i < table.count; ++i)
        {
            const AotScript& script = table.scripts[i];
            Registry reg;
            script.register_fn(reg);
            for (const auto& [name, e] : reg.entries())
            {
                if (registry_.find(name))
                    throw std::runtime_error("the class " + name + " is in two AOT scripts (" + script.name + ")");
                registry_.add(e.record, e.create);
            }
            for (const auto& [key, t] : reg.thunks())
                registry_.add_thunk(t);
            const ModuleStateInfo& ms = reg.module_state();
            if (ms.layout && ms.slot)
            {
                char* block = alloc_state(ms.layout);
                if (!block)
                    throw std::bad_alloc();
                *ms.slot = block; // an initializer can read the block
                try
                {
                    for (uint32_t f = 0; f < ms.layout->field_count; ++f)
                    {
                        const FieldInfo& fi = ms.layout->fields[f];
                        if (fi.init)
                            fi.init(block, block + fi.offset);
                    }
                }
                catch (...)
                {
                    *ms.slot = nullptr;
                    aligned_free(block); // the partial state is unknown: no destructor call
                    throw;
                }
                module_blocks_.push_back(ms);
            }
            ++script_count_;
        }
    }
    catch (...)
    {
        for (const ModuleStateInfo& info : module_blocks_)
            destroy_module_block(info);
        module_blocks_.clear();
        throw;
    }
}

AotRegistry::~AotRegistry()
{
    for (auto it = module_blocks_.rbegin(); it != module_blocks_.rend(); ++it)
        destroy_module_block(*it);
}

std::vector<std::string> AotRegistry::class_names() const
{
    std::vector<std::string> names;
    for (const auto& [name, e] : registry_.entries())
        names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

const ClassRecord* AotRegistry::record(const std::string& class_name) const
{
    const ClassEntry* e = registry_.find(class_name);
    return e ? e->record : nullptr;
}

Instance AotRegistry::create(const std::string& class_name) const
{
    const ClassEntry* e = registry_.find(class_name);
    if (!e)
        throw std::runtime_error("no AOT script class " + class_name);
    return e->create();
}

} // namespace hot
