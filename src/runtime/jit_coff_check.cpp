// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A check of the unwind data of COFF x64 JIT code on each host (hot_jit_coff_unwind_check). The
// code goes to a COFF object. RuntimeDyld loads it with CoffMemoryManager, and a hook records the
// tables instead of RtlAddFunctionTable. The external symbols get addresses far from the JIT
// memory, as a DLL of the host. The code never runs.

#include "jit_coff_check.h"

#include "jit_coff_memory.h"

#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/RuntimeDyld.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hot::jit
{
namespace
{

struct Table
{
    const uint8_t* entries;
    uint32_t count;
    uint64_t base;
};

bool record_add(void* table, uint32_t count, uint64_t base, void* ctx)
{
    static_cast<std::vector<Table>*>(ctx)->push_back({static_cast<const uint8_t*>(table), count, base});
    return true;
}

void record_remove(void*, void*) {}

// Each external symbol gets its own address in a range far from the JIT memory (more than 4 GB).
struct FarResolver : llvm::LegacyJITSymbolResolver
{
    std::map<std::string, uint64_t> addresses;
    uint64_t next = 0x100000000000ull;
    llvm::JITSymbol findSymbol(const std::string& name) override
    {
        auto [it, inserted] = addresses.try_emplace(name, next);
        if (inserted)
            next += 0x100;
        return llvm::JITSymbol(it->second, llvm::JITSymbolFlags::Exported);
    }
    llvm::JITSymbol findSymbolInLogicalDylib(const std::string&) override { return nullptr; }
    std::string name_of(uint64_t a) const
    {
        for (const auto& [name, value] : addresses)
            if (value == a)
                return name;
        return std::string();
    }
};

uint32_t read32(uintptr_t a)
{
    uint32_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), 4);
    return v;
}

uint64_t read64(uintptr_t a)
{
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), 8);
    return v;
}

class Checker
{
public:
    Checker(const CoffMemoryManager& mm, const FarResolver& resolver, std::string& errors)
        : mm_(mm), resolver_(resolver), errors_(errors)
    {
    }

    // The block of the address, or null.
    const CoffMemoryManager::Block* block_of(uintptr_t a) const
    {
        for (const CoffMemoryManager::Block& b : mm_.blocks())
            if (a >= b.addr && a < b.addr + b.size)
                return &b;
        return nullptr;
    }

    // The image-relative offset "rva" must point into a block (of code when "code").
    bool rva_in(uint64_t base, uint32_t rva, std::optional<bool> code, const std::string& what)
    {
        const uintptr_t a = static_cast<uintptr_t>(base + rva);
        const CoffMemoryManager::Block* b = block_of(a);
        if (!b)
            return fail(what + " RVA 0x" + llvm::utohexstr(rva) + " is outside each section");
        if (code && b->code != *code)
            return fail(what + " RVA 0x" + llvm::utohexstr(rva) + (*code ? " is not code" : " is in code memory"));
        return true;
    }

    // The name of the external function of a jump stub (RuntimeDyld: jmp *0(%rip) and the address).
    std::string stub_target(uintptr_t a) const
    {
        const auto* p = reinterpret_cast<const uint8_t*>(a);
        if (p[0] != 0xFF || p[1] != 0x25 || read32(a + 2) != 0)
            return std::string();
        return resolver_.name_of(read64(a + 6));
    }

    void check_func_info(uint64_t base, uint32_t rva)
    {
        if (!rva_in(base, rva, std::nullopt, "FuncInfo"))
            return;
        const uintptr_t f = static_cast<uintptr_t>(base + rva);
        const uint32_t magic = read32(f) & 0x1FFFFFFF;
        if (magic < 0x19930520 || magic > 0x19930522)
            fail("FuncInfo magic 0x" + llvm::utohexstr(magic));
        const uint32_t max_state = read32(f + 4), unwind_map = read32(f + 8), n_try = read32(f + 12);
        const uint32_t try_map = read32(f + 16), n_ip = read32(f + 20), ip_map = read32(f + 24);
        if (max_state && rva_in(base, unwind_map, std::nullopt, "UnwindMap"))
            for (uint32_t i = 0; i < max_state; ++i)
                if (const uint32_t action = read32(static_cast<uintptr_t>(base + unwind_map) + 8 * i + 4))
                    rva_in(base, action, true, "unwind action");
        if (n_ip && rva_in(base, ip_map, std::nullopt, "IPtoStateMap"))
            for (uint32_t i = 0; i < n_ip; ++i)
                rva_in(base, read32(static_cast<uintptr_t>(base + ip_map) + 8 * i), true, "IP of a state");
        if (n_try && rva_in(base, try_map, std::nullopt, "TryBlockMap"))
            for (uint32_t i = 0; i < n_try; ++i)
            {
                ++try_blocks;
                const uintptr_t t = static_cast<uintptr_t>(base + try_map) + 20 * i;
                const uint32_t n_catch = read32(t + 12), handlers = read32(t + 16);
                if (!rva_in(base, handlers, std::nullopt, "HandlerArray"))
                    continue;
                for (uint32_t c = 0; c < n_catch; ++c)
                {
                    const uintptr_t h = static_cast<uintptr_t>(base + handlers) + 20 * c;
                    const uint32_t type = read32(h + 4), handler = read32(h + 12);
                    rva_in(base, handler, true, "catch handler");
                    if (type == 0) // catch (...)
                        continue;
                    ++catch_types;
                    // Data, not code: a jump stub in place of the TypeDescriptor is an error.
                    if (rva_in(base, type, false, "TypeDescriptor"))
                    {
                        // The first field of a TypeDescriptor is the address of the type_info vftable.
                        const std::string vft = resolver_.name_of(read64(static_cast<uintptr_t>(base + type)));
                        if (vft != "??_7type_info@@6B@")
                            fail("TypeDescriptor RVA 0x" + llvm::utohexstr(type) + " has no type_info vftable");
                    }
                }
            }
    }

    void check_entry(const Table& t, uint32_t i)
    {
        const uintptr_t e = reinterpret_cast<uintptr_t>(t.entries) + 12 * i;
        const uint32_t begin = read32(e), end = read32(e + 4), unwind = read32(e + 8);
        const std::string at = "entry " + std::to_string(i) + " [0x" + llvm::utohexstr(begin) + ", 0x" +
                               llvm::utohexstr(end) + ")";
        ++entries;
        if (begin >= end)
            fail(at + ": the begin is not before the end");
        if (!rva_in(t.base, begin, true, at + " begin") || !rva_in(t.base, end - 1, true, at + " end"))
            return;
        if (block_of(t.base + begin) != block_of(t.base + end - 1))
            fail(at + ": the function is in two sections");
        if (begin < last_begin_)
            fail(at + ": the entries are not sorted");
        last_begin_ = begin;
        if ((unwind & 3) != 0 || !rva_in(t.base, unwind, std::nullopt, at + " UNWIND_INFO"))
            return;
        const uintptr_t u = static_cast<uintptr_t>(t.base + unwind);
        const auto* p = reinterpret_cast<const uint8_t*>(u);
        const unsigned version = p[0] & 7, flags = p[0] >> 3, codes = p[2];
        if (version != 1 && version != 2)
            fail(at + ": UNWIND_INFO version " + std::to_string(version));
        if (p[1] > end - begin)
            fail(at + ": the prolog is longer than the function");
        const uintptr_t tail = u + 4 + 2 * ((codes + 1u) & ~1u);
        if (flags & 4) // UNW_FLAG_CHAININFO: a RUNTIME_FUNCTION follows
        {
            rva_in(t.base, read32(tail), true, at + " chained begin");
            rva_in(t.base, read32(tail + 8), std::nullopt, at + " chained UNWIND_INFO");
        }
        else if (flags & 3) // UNW_FLAG_EHANDLER or UNW_FLAG_UHANDLER
        {
            ++handlers;
            const uint32_t handler = read32(tail);
            if (!rva_in(t.base, handler, true, at + " handler"))
                return;
            const std::string name = stub_target(static_cast<uintptr_t>(t.base + handler));
            if (name.empty())
                fail(at + ": the handler is not a stub of a host function");
            if (name == "__CxxFrameHandler3")
                check_func_info(t.base, read32(tail + 4));
        }
    }

    void check(const std::vector<Table>& tables, const std::map<uintptr_t, std::string>& functions)
    {
        for (const Table& t : tables)
        {
            ++this->tables;
            last_begin_ = 0;
            if (t.base != mm_.low())
                fail("the table base 0x" + llvm::utohexstr(t.base) + " is not the lowest section 0x" +
                     llvm::utohexstr(mm_.low()));
            if (mm_.high() - t.base > UINT32_MAX)
                fail("the image is larger than 4 GB");
            for (uint32_t i = 0; i < t.count; ++i)
            {
                check_entry(t, i);
                const uintptr_t e = reinterpret_cast<uintptr_t>(t.entries) + 12 * i;
                const uintptr_t lo = static_cast<uintptr_t>(t.base + read32(e)), hi = static_cast<uintptr_t>(t.base + read32(e + 4));
                // A function symbol in the entry must be at its begin.
                for (auto it = functions.lower_bound(lo); it != functions.end() && it->first < hi; ++it)
                    if (it->first != lo)
                        fail("the function " + it->second + " starts inside an entry");
                if (functions.count(lo))
                    ++named_entries;
            }
        }
    }

    unsigned tables = 0, entries = 0, named_entries = 0, handlers = 0, try_blocks = 0, catch_types = 0;

private:
    bool fail(const std::string& text)
    {
        errors_ += text + "\n";
        return false;
    }

    const CoffMemoryManager& mm_;
    const FarResolver& resolver_;
    std::string& errors_;
    uint32_t last_begin_ = 0;
};

#if defined(HOT_JIT_HAS_X86_TARGET)
extern "C" void LLVMInitializeX86TargetInfo();
extern "C" void LLVMInitializeX86Target();
extern "C" void LLVMInitializeX86TargetMC();
extern "C" void LLVMInitializeX86AsmPrinter();

// The JIT registers only the native target. On other hosts (for example arm64) the check
// registers the X86 target itself. A second registration has no effect.
void register_x86_target()
{
    static const bool done = [] {
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        return true;
    }();
    (void)done;
}
#endif

} // namespace

std::string check_coff_unwind(llvm::Module& m)
{
    const llvm::Triple triple(m.getTargetTriple());
    if (!triple.isOSBinFormatCOFF() || triple.getArch() != llvm::Triple::x86_64)
        return "error: the target is not COFF x86_64\n";
#if defined(HOT_JIT_HAS_X86_TARGET)
    register_x86_target();
#else
    return "skip: the LLVM libraries have no X86 target\n";
#endif
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target)
        return "error: " + error + "\n";
    // The options of the JIT target machine (JITTargetMachineBuilder): emulated TLS.
    llvm::TargetOptions options;
    options.EmulatedTLS = true;
    std::unique_ptr<llvm::TargetMachine> tm(
        target->createTargetMachine(triple, "", "", options, std::nullopt, std::nullopt));
    if (!tm)
        return "error: no target machine\n";
    m.setDataLayout(tm->createDataLayout());
    llvm::SmallVector<char, 0> buffer;
    {
        llvm::raw_svector_ostream os(buffer);
        llvm::legacy::PassManager pm;
        if (tm->addPassesToEmitFile(pm, os, nullptr, llvm::CodeGenFileType::ObjectFile))
            return "error: the target cannot emit an object\n";
        pm.run(m);
    }
    auto object = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(llvm::StringRef(buffer.data(), buffer.size()), "coff-check"));
    if (!object)
        return "error: " + llvm::toString(object.takeError()) + "\n";

    std::vector<Table> tables;
    CoffMemoryManager mm(UnwindTableHooks{record_add, record_remove, &tables});
    FarResolver resolver;
    llvm::RuntimeDyld dyld(mm, resolver);
    auto info = dyld.loadObject(**object);
    if (dyld.hasError())
        return "error: " + dyld.getErrorString().str() + "\n";
    dyld.resolveRelocations();
    dyld.registerEHFrames();
    if (dyld.hasError())
        return "error: " + dyld.getErrorString().str() + "\n";

    std::map<uintptr_t, std::string> functions;
    for (const llvm::object::SymbolRef& s : (*object)->symbols())
    {
        auto type = s.getType();
        auto name = s.getName();
        if (!type || !name || *type != llvm::object::SymbolRef::ST_Function)
        {
            if (!type)
                llvm::consumeError(type.takeError());
            if (!name)
                llvm::consumeError(name.takeError());
            continue;
        }
        const llvm::JITEvaluatedSymbol sym = dyld.getSymbol(*name);
        if (sym.getAddress() != 0)
            functions.emplace(static_cast<uintptr_t>(sym.getAddress()), name->str());
    }

    std::string errors;
    Checker checker(mm, resolver, errors);
    checker.check(tables, functions);
    std::string text = "tables " + std::to_string(checker.tables) + " entries " + std::to_string(checker.entries) +
                       " named " + std::to_string(checker.named_entries) + " handlers " +
                       std::to_string(checker.handlers) + " try " + std::to_string(checker.try_blocks) +
                       " catch_types " + std::to_string(checker.catch_types) + " sections " +
                       std::to_string(mm.blocks().size()) + "\n";
    std::string error_text;
    if (mm.finalizeMemory(&error_text)) // true: an error
        errors += "finalizeMemory: " + error_text + "\n";
    return text + errors;
}

} // namespace hot::jit
