// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The memory manager of the COFF x64 JIT code (src/runtime/jit_engine.cpp). The file has no
// Windows header: the system calls of the unwind table go through hooks. So a test on each host
// can load a COFF object with this manager and check the tables (hot_jit_coff_unwind_check).

#pragma once

#include <llvm/ExecutionEngine/SectionMemoryManager.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace hot::jit
{

// The address ranges of the loaded JIT objects: the first address to the end. Each object is
// one image for the unwind data and for the throw records. The map is never destroyed, because
// a module can go away after the static destructors of this DLL.
struct ImageRanges
{
    std::mutex mutex;
    std::map<uintptr_t, uintptr_t> ranges;
};

inline ImageRanges& image_ranges()
{
    static ImageRanges* ranges = new ImageRanges;
    return *ranges;
}

// The image base of the JIT object that holds "p". Zero when "p" is not in JIT memory.
inline uintptr_t jit_image_base(const void* p)
{
    const auto a = reinterpret_cast<uintptr_t>(p);
    ImageRanges& r = image_ranges();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto it = r.ranges.upper_bound(a);
    if (it == r.ranges.begin())
        return 0;
    --it;
    return a < it->second ? it->first : 0;
}

// The calls that add and remove an unwind table: RtlAddFunctionTable and RtlDeleteFunctionTable
// on Windows. "add" gets the RUNTIME_FUNCTION records, their count and the image base.
struct UnwindTableHooks
{
    bool (*add)(void* table, uint32_t count, uint64_t base, void* ctx) = nullptr;
    void (*remove)(void* table, void* ctx) = nullptr;
    void* ctx = nullptr;
};

// The memory manager of one COFF object. RuntimeDyld resolves an image-relative relocation
// (ADDR32NB, for example in .pdata and .xdata) against the lowest section address of the object
// (RuntimeDyldCOFFX86_64::getImageBase). The unwind table of the system uses the same base.
// - One reserved block holds all sections. So each REL32 and each ADDR32NB offset fits.
// - The block has code first, then read-only data, then data. The first code section is the lowest
//   address. RuntimeDyld computes the base early (a stub of .xdata), after the code sections.
// - .xdata goes into the code memory. RuntimeDyld puts the jump stub of an external symbol (the
//   personality routine, a copy constructor of a throw record) into the section of the
//   relocation. The stub must be executable.
class CoffMemoryManager : public llvm::SectionMemoryManager
{
public:
    // One section of the object: its address, its size, and its kind.
    struct Block
    {
        uintptr_t addr;
        uintptr_t size;
        bool code;
    };

    explicit CoffMemoryManager(UnwindTableHooks hooks) : llvm::SectionMemoryManager(nullptr, true), hooks_(hooks) {}
    ~CoffMemoryManager() override { release(); }

    void reserveAllocationSpace(uintptr_t code_size, llvm::Align code_align, uintptr_t ro_size,
        llvm::Align ro_align, uintptr_t rw_size, llvm::Align rw_align) override
    {
        if (code_size == 0 && ro_size == 0 && rw_size == 0)
            return;
        // A section takes one alignment more than its size (SectionMemoryManager::allocateSection),
        // and each section is at least one alignment. Two times the size is enough for each part.
        const llvm::Align align = std::max(code_align, ro_align);
        const uintptr_t slack = 4096;
        llvm::SectionMemoryManager::reserveAllocationSpace(2 * (code_size + ro_size) + slack, align,
            2 * ro_size + slack, ro_align, 2 * rw_size + slack, rw_align);
    }

    uint8_t* allocateCodeSection(
        uintptr_t size, unsigned alignment, unsigned section_id, llvm::StringRef name) override
    {
        return note(llvm::SectionMemoryManager::allocateCodeSection(size, alignment, section_id, name), size, true);
    }

    uint8_t* allocateDataSection(uintptr_t size, unsigned alignment, unsigned section_id,
        llvm::StringRef name, bool read_only) override
    {
        if (name == ".xdata")
            return allocateCodeSection(size, alignment, section_id, name);
        return note(
            llvm::SectionMemoryManager::allocateDataSection(size, alignment, section_id, name, read_only), size,
            false);
    }

    // RuntimeDyldCOFFX86_64 gives each .pdata section here. The entries are RUNTIME_FUNCTION
    // records with offsets from the image base.
    void registerEHFrames(uint8_t* addr, uint64_t, size_t size) override
    {
        const auto count = static_cast<uint32_t>(size / 12); // sizeof(RUNTIME_FUNCTION)
        if (count > 0 && hooks_.add && hooks_.add(addr, count, low_, hooks_.ctx))
            tables_.push_back(addr);
    }

    void deregisterEHFrames() override { release(); }

    bool finalizeMemory(std::string* error) override
    {
        if (!registered_ && low_ < high_)
        {
            ImageRanges& r = image_ranges();
            std::lock_guard<std::mutex> lock(r.mutex);
            r.ranges[low_] = high_;
            registered_ = true;
        }
        return llvm::SectionMemoryManager::finalizeMemory(error);
    }

    uintptr_t low() const { return low_; }
    uintptr_t high() const { return high_; }
    const std::vector<Block>& blocks() const { return blocks_; }

private:
    uint8_t* note(uint8_t* p, uintptr_t size, bool code)
    {
        if (p)
        {
            const auto a = reinterpret_cast<uintptr_t>(p);
            low_ = std::min(low_, a);
            high_ = std::max(high_, a + size);
            blocks_.push_back({a, size, code});
        }
        return p;
    }

    void release()
    {
        for (void* table : tables_)
            if (hooks_.remove)
                hooks_.remove(table, hooks_.ctx);
        tables_.clear();
        if (registered_)
        {
            ImageRanges& r = image_ranges();
            std::lock_guard<std::mutex> lock(r.mutex);
            r.ranges.erase(low_);
            registered_ = false;
        }
    }

    UnwindTableHooks hooks_;
    uintptr_t low_ = UINTPTR_MAX;
    uintptr_t high_ = 0;
    bool registered_ = false;
    std::vector<void*> tables_;
    std::vector<Block> blocks_;
};

} // namespace hot::jit
