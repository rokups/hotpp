// bench_backends: measure the run-time cost of the script backends and the cost of a load.
//
// One binary tests every backend that the build has, at run time, with no rebuild:
//   files  the clang files backend (a compiler process for each load)
//   jit    the in-process JIT backend
//   aot    the C++ of the scripts is part of this binary (Loader::load_aot)
// A pure native C++ twin of each script class is the reference.
//
// Usage:
//   bench_backends [--backend all|files|jit|aot] [--filter text] [--iters N] [--list]
//                  [--csv file] [--warm N] [--reps N] [--opt FLAG] [--cxxflag F]...
//                  [--no-cache] [--no-load] [--reloads N]
//
//   --backend  the backend to test. The default "all" tests every backend that the build has.
//              A backend that the build does not have is skipped with a note.
//   --filter   run only the cases whose name contains the text. The load section runs only
//              when there is no filter.
//   --iters N  the loop count of each case (the default is the count of the case).
//   --list     print the backends and the cases, then exit.
//   --csv FILE write the table also as CSV.
//   --warm N   the number of warm loads in the load section (default 2).
//   --reps N   the number of timed runs of a case. The result is the median (default 7).
//   --opt FLAG the optimization flag for the scripts that a backend compiles (default -O2).
//              The native twin and the AOT scripts get -O2 from CMake.
//   --cxxflag F add a compile flag for the scripts (repeat it for more flags).
//   --no-cache each warm load of the load section gets a new translator session.
//   --no-load  skip the load section.
//   --reloads N the largest N of the reload chain (default 100). The chain measures a call after
//              0, 1, 10 and 100 reloads. Each reload needs one compile.
//
// Each case is a script in tools/bench/scripts and a native twin in this file. Both derive from
// BenchBase (BenchEngine.h) and have the same algorithm. The host calls them through BenchBase.
// The result of each case is checked against the result of the twin.
//
// CMake defines: BENCH_SOURCE_DIR, BENCH_SCRIPT_DIR, BENCH_CXX, BENCH_INCLUDES (items split by
// "|"), BENCH_RESOURCE_DIR and BENCH_WORK_DIR. The macros HOT_WITH_CLANG_FILES and HOT_WITH_JIT
// come from hot_backend.
#include "BenchEngine.h"
#include "hot/aot.h"
#include "hot/backend.h"
#include "hot/config.h"
#include "hot/hotc_lib.h"
#include "hot/runtime.h"
#include "hot/runtime/loader.h"
#include "hot/scratch.h"

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(HOT_WITH_JIT)
    #include "hot/jit_backend.h"
#endif
#if defined(HOT_WITH_CLANG_FILES)
    #include "hot/clang_files_backend.h"
#endif

namespace
{

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, '|');)
        if (!item.empty())
            v.push_back(item);
    return v;
}

std::string read_file(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Keep the compiler from removing the work of the native twin.
volatile int64_t g_sink = 0;

template <class T> inline void escape(T* p)
{
    asm volatile("" : : "g"(p) : "memory");
}

#define BENCH_NOINLINE __attribute__((noinline))

// ---- The native twins. Each one does the same work as the script of the same name. ----

struct NEmpty : BenchBase
{
    int64_t Run(int n) override { return n; }
};

struct NCallScript : BenchBase
{
    // Recursive, so the compiler cannot remove the calls (as in the script).
    BENCH_NOINLINE int Fib(int n) { return n < 2 ? n : Fib(n - 1) + Fib(n - 2); }
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += Fib(10 + (i & 1));
        return s;
    }
};

struct NCallNative : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += NativeAdd(i, 1);
        return s;
    }
};

struct NHook : BenchBase
{
    int Hook(int x) override { return x + 1; }
    int64_t Run(int n) override { return DriveHook(n); }
};

struct NFields : BenchBase
{
    int a = 1;
    int b = 2;
    float f = 0.5f;
    int64_t Run(int n) override
    {
        for (int i = 0; i < n; i++)
        {
            a = (a + i) & 65535;
            b = b ^ a;
            f = f + 0.25f;
        }
        return a + b + static_cast<int64_t>(f);
    }
};

struct NArithInt : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        int x = 1;
        for (int i = 0; i < n; i++)
        {
            x = (x * 31 + i) & 1048575;
            s += x % 7 + (x >> 3);
        }
        return s;
    }
};

struct NArithFloat : BenchBase
{
    int64_t Run(int n) override
    {
        float x = 0.5f;
        double d = 0.0;
        for (int i = 0; i < n; i++)
        {
            x = x * 0.999f + 0.5f;
            d = d + x * 0.25;
        }
        return static_cast<int64_t>(d);
    }
};

struct NArray : BenchBase
{
    std::array<int, 256> arr{};
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            int k = i & 255;
            arr[k] = (arr[(k + 1) & 255] + i) & 65535;
            s += arr[k];
        }
        return s;
    }
};

struct NVector : BenchBase
{
    std::vector<int> v;
    int64_t Run(int n) override
    {
        if (v.size() == 0)
            for (int j = 0; j < 256; j++)
                v.push_back(j);
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            int k = i & 255;
            v[k] = (v[(k + 1) & 255] + i) & 65535;
            s += v[k];
        }
        return s;
    }
};

struct NNode
{
    int v = 0;
    std::shared_ptr<NNode> next;
};

struct NHandles : BenchBase
{
    std::shared_ptr<NNode> head;
    int64_t Run(int n) override
    {
        if (!head)
        {
            head = std::make_shared<NNode>();
            head->v = 1;
            std::shared_ptr<NNode> cur = head;
            for (int j = 1; j < 4; j++)
            {
                cur->next = std::make_shared<NNode>();
                cur->next->v = j + 1;
                cur = cur->next;
            }
        }
        int64_t s = 0;
        std::shared_ptr<NNode> p = head;
        for (int i = 0; i < n; i++)
        {
            if (!p)
                p = head;
            s += p->v;
            p = p->next;
        }
        return s;
    }
};

struct NString : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            std::string t = "v=" + std::to_string(i) + "-" + std::to_string(s);
            escape(t.data());
            s += static_cast<int64_t>(t.size());
        }
        return s;
    }
};

struct NObj
{
    int a = 1;
    int b = 2;
};

struct NAlloc : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            std::shared_ptr<NObj> o = std::make_shared<NObj>();
            escape(o.get());
            o->a = i;
            s += o->a + o->b;
        }
        return s;
    }
};

struct NClosure : BenchBase
{
    int64_t Run(int n) override
    {
        int k = 3;
        std::function<int(int)> f = [k](int x) { return x + k; };
        escape(&f);
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += f(i);
        return s;
    }
};

struct NMethodLoop : BenchBase
{
    int a = 0;
    int b = 0;
    void Step(int d)
    {
        a = (a + d) & 65535;
        b = b + a;
        if (b > 1000000)
            b = b - 1000000;
    }
    int64_t Run(int n) override
    {
        for (int i = 0; i < n; i++)
            Step(i);
        return a + b;
    }
};

// ---- new and free: the native kinds of smart pointer (native-only rows) ----
// Each twin does the work of NAlloc with another kind of owner. The table compares the script
// case "alloc" with each of them.

struct NAllocSharedNew : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            std::shared_ptr<NObj> o(new NObj);
            escape(o.get());
            o->a = i;
            s += o->a + o->b;
        }
        return s;
    }
};

struct NAllocUnique : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            std::unique_ptr<NObj> o = std::make_unique<NObj>();
            escape(o.get());
            o->a = i;
            s += o->a + o->b;
        }
        return s;
    }
};

// An intrusive pointer with an atomic count in the object (as boost::intrusive_ptr).
struct IObj
{
    std::atomic<int> refs{0};
    int a = 1;
    int b = 2;
};

class IPtr
{
public:
    explicit IPtr(IObj* p) : p_(p) { p_->refs.fetch_add(1, std::memory_order_relaxed); }
    IPtr(const IPtr&) = delete;
    IPtr& operator=(const IPtr&) = delete;
    ~IPtr()
    {
        if (p_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete p_;
    }
    IObj* operator->() const { return p_; }
    IObj* get() const { return p_; }

private:
    IObj* p_;
};

struct NAllocIntrusive : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            IPtr o(new IObj);
            escape(o.get());
            o->a = i;
            s += o->a + o->b;
        }
        return s;
    }
};

// A twin with the shape of a script object: a shell with a virtual base and a separate state
// block. The constructor allocates the block (aligned_alloc) and runs the field initializers
// through a table of function pointers. A function pointer makes the shell, as the record of a
// script class does. A std::shared_ptr made from "new" owns the shell. With "Track", the twin
// also adds the object to a hash map of live objects and removes it in the destructor.
struct NShellBase
{
    const void* cls = nullptr;
    void* state = nullptr;
};

struct NPlain : std::enable_shared_from_this<NPlain>
{
    virtual ~NPlain() = default;
};

struct NObjState
{
    NShellBase* owner;
    int a;
    int b;
};

struct NFieldInit
{
    size_t offset;
    void (*init)(void*);
};

void n_init_a(void* f)
{
    new (f) int(1);
}
void n_init_b(void* f)
{
    new (f) int(2);
}
const NFieldInit kNObjFields[] = {{offsetof(NObjState, a), n_init_a}, {offsetof(NObjState, b), n_init_b}};

std::unordered_map<NShellBase*, uint32_t> g_native_live;

template <bool Track> struct NShellObj final : NPlain, NShellBase
{
    NShellObj()
    {
        constexpr size_t align = alignof(NObjState);
        constexpr size_t size = (sizeof(NObjState) + align - 1) / align * align;
        char* mem = static_cast<char*>(std::aligned_alloc(align, size));
        if (!mem)
            throw std::bad_alloc();
        *reinterpret_cast<NShellBase**>(mem) = this;
        for (const NFieldInit& f : kNObjFields)
            f.init(mem + f.offset);
        state = mem;
        if constexpr (Track)
            g_native_live.emplace(this, 0u);
    }
    ~NShellObj() override
    {
        if constexpr (Track)
            g_native_live.erase(this);
        std::free(state);
    }
};

template <bool Track> BENCH_NOINLINE NShellBase* n_new_shell()
{
    auto* s = new NShellObj<Track>();
    if constexpr (Track)
    {
        auto it = g_native_live.find(s); // the creator note of construct_shell
        if (it != g_native_live.end())
            it->second = 1;
    }
    return s;
}

template <bool Track> struct NAllocShell : BenchBase
{
    NShellBase* (*make)() = n_new_shell<Track>;
    int64_t Run(int n) override
    {
        escape(&make);
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            std::shared_ptr<NShellObj<Track>> o(static_cast<NShellObj<Track>*>(make()));
            escape(o.get());
            NObjState* st = static_cast<NObjState*>(o->state);
            st->a = i;
            s += st->a + st->b;
        }
        return s;
    }
};

// ---- Twins of the cases of reloadable access and calls ----

struct NCell
{
    int x = 0;
    int y = 1;
};

struct NStateField : BenchBase
{
    std::shared_ptr<NCell> c;
    int64_t Run(int n) override
    {
        if (!c)
            c = std::make_shared<NCell>();
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            c->x = (c->x + i) & 65535;
            c->y = c->y ^ c->x;
            s += c->y;
        }
        return s;
    }
};

struct NEntry : BenchBase
{
    int k = 1;
    int Add(int x) { return x + k; }
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += Add(i ^ static_cast<int>(s & 7)); // a loop-carried value: no closed form
        return s;
    }
};

struct NShape
{
    int w = 2;
    virtual ~NShape() = default;
    virtual int Area(int x) { return x; }
};

struct NSquare : NShape
{
    int Area(int x) override { return x * w + 1; }
};

struct NSlotScript : BenchBase
{
    std::shared_ptr<NShape> s;
    int64_t Run(int n) override
    {
        if (!s)
        {
            s = std::make_shared<NSquare>();
            escape(s.get()); // the compiler must not see the dynamic type
        }
        NShape* p = s.get();
        escape(&p);
        int64_t t = 0;
        for (int i = 0; i < n; i++)
            t += p->Area(i & 255);
        return t;
    }
};

struct NAdder
{
    int k = 3;
    int Add(int x) { return x + k; }
};

struct NHandleCall : BenchBase
{
    std::shared_ptr<NAdder> a;
    int64_t Run(int n) override
    {
        if (!a)
            a = std::make_shared<NAdder>();
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += a->Add(i ^ static_cast<int>(s & 7));
        return s;
    }
};

int g_counter = 0;
int g_step = 3;

struct NModVar : BenchBase
{
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
        {
            g_counter = (g_counter + g_step + i) & 65535;
            s += g_counter;
        }
        return s;
    }
};

int g_bias = 1;

int n_add_bias(int x)
{
    return x + g_bias;
}

struct NModFn : BenchBase
{
    static int Twice(int x) { return x * 2; }
    int64_t Run(int n) override
    {
        int64_t s = 0;
        for (int i = 0; i < n; i++)
            s += n_add_bias(i) + Twice(i & 7);
        return s;
    }
};

// The twin of the reload chain: a native to script virtual call that reads a field.
struct NChain : BenchBase
{
    int k = 1;
    int Hook(int x) override { return x + k; }
    int64_t Run(int n) override { return DriveHook(n); }
};

template <class T> std::unique_ptr<BenchBase> make_native()
{
    return std::make_unique<T>();
}

struct Case
{
    const char* name;
    const char* label;
    const char* script; // the file in BENCH_SCRIPT_DIR
    const char* cls;    // the script class
    std::unique_ptr<BenchBase> (*make)();
    int64_t iters;
    bool per_call; // true: the host calls Run(0) iters times. false: one call with n = iters.
    double ops_per_iter; // the operations in one iteration (the divisor of the time)
};

const Case kCases[] = {
    {"empty", "empty method call (host to script)", "empty.hot", "BenchEmpty", make_native<NEmpty>, 5000000, true, 1},
    {"call_script", "script to script call", "call_script.hot", "BenchCallScript", make_native<NCallScript>, 20000,
        false, 232},
    {"call_native", "script to native call", "call_native.hot", "BenchCallNative", make_native<NCallNative>, 5000000,
        false, 1},
    {"virtual_hook", "native to script virtual call", "virtual_hook.hot", "BenchHook", make_native<NHook>, 5000000,
        false, 1},
    {"fields", "field read and write loop", "fields.hot", "BenchFields", make_native<NFields>, 5000000, false, 1},
    {"arith_int", "integer arithmetic loop", "arith_int.hot", "BenchArithInt", make_native<NArithInt>, 5000000, false, 1},
    {"arith_float", "float arithmetic loop", "arith_float.hot", "BenchArithFloat", make_native<NArithFloat>, 5000000,
        false, 1},
    {"array", "array index loop (T[256], checked)", "array.hot", "BenchArray", make_native<NArray>, 5000000, false, 1},
    {"vector", "std.vector index loop", "vector.hot", "BenchVector", make_native<NVector>, 5000000, false, 1},
    {"handles", "handle deref and null check chain", "handles.hot", "BenchHandles", make_native<NHandles>, 3000000,
        false, 1},
    {"string", "string interpolation / concat", "string.hot", "BenchString", make_native<NString>, 300000, false, 1},
    {"alloc", "new and free of a script object", "alloc.hot", "BenchAlloc", make_native<NAlloc>, 1000000, false, 1},
    {"closure", "closure call", "closure.hot", "BenchClosure", make_native<NClosure>, 5000000, false, 1},
    {"method_loop", "loop with a method call", "method_loop.hot", "BenchMethodLoop", make_native<NMethodLoop>, 5000000,
        false, 1},
    // Native-only rows: the kinds of owner for the comparison with "alloc".
    {"alloc_shared_new", "native: std::shared_ptr<T>(new T)", nullptr, nullptr, make_native<NAllocSharedNew>, 1000000,
        false, 1},
    {"alloc_unique", "native: std::make_unique<T>()", nullptr, nullptr, make_native<NAllocUnique>, 1000000, false, 1},
    {"alloc_intrusive", "native: intrusive pointer (atomic count)", nullptr, nullptr, make_native<NAllocIntrusive>,
        1000000, false, 1},
    {"alloc_shell", "native: virtual base + state block", nullptr, nullptr, make_native<NAllocShell<false>>, 1000000,
        false, 1},
    {"alloc_shell_map", "native: shell + hash map of live objects", nullptr, nullptr, make_native<NAllocShell<true>>,
        1000000, false, 1},
    // Access and calls of reloadable code.
    {"state_field", "field of another object (state block)", "state_field.hot", "BenchStateField",
        make_native<NStateField>, 5000000, false, 1},
    {"method_entry", "small method call on this (live check)", "method_entry.hot", "BenchEntry", make_native<NEntry>,
        5000000, false, 1},
    {"slot_script", "script to script virtual call (slot)", "slot_script.hot", "BenchSlotScript",
        make_native<NSlotScript>, 5000000, false, 1},
    {"handle_call", "method call through a handle", "handle_call.hot", "BenchHandleCall", make_native<NHandleCall>,
        5000000, false, 1},
    {"module_var", "module variable read and write", "module_var.hot", "BenchModVar", make_native<NModVar>, 5000000,
        false, 1},
    {"module_fn", "module function + static method call", "module_fn.cpp", "BenchModFn", make_native<NModFn>, 5000000,
        false, 1},
};

// ---- The measurement ----

// The result of one timed run is ns for each operation.
BENCH_NOINLINE double time_run(BenchBase& b, const Case& c, int64_t iters)
{
    int64_t sink = 0;
    const Clock::time_point t = Clock::now();
    if (c.per_call)
        for (int64_t i = 0; i < iters; ++i)
            sink += b.Run(0);
    else
        sink = b.Run(static_cast<int>(iters));
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - t).count();
    g_sink = sink;
    return ns / (static_cast<double>(iters) * c.ops_per_iter);
}

BENCH_NOINLINE int64_t one_result(BenchBase& b)
{
    return b.Run(1000);
}

struct Options
{
    std::string backend = "all";
    std::string filter;
    std::string csv;
    int64_t iters = 0;
    int warm = 2;
    int reps = 7;
    std::string opt = "-O2";
    std::vector<std::string> flags; // more compile flags for the scripts (--cxxflag)
    bool no_cache = false;
    bool list = false;
    bool no_load = false;
    int reloads = 100; // the largest N of the reload chain
};

// The median of the timed runs, after two warm-up runs.
double measure(BenchBase& b, const Case& c, const Options& o)
{
    const int64_t iters = o.iters > 0 ? o.iters : c.iters;
    time_run(b, c, std::max<int64_t>(iters / 4, 1));
    time_run(b, c, std::max<int64_t>(iters / 4, 1));
    std::vector<double> v;
    for (int i = 0; i < o.reps; ++i)
        v.push_back(time_run(b, c, iters));
    return median(v);
}

// ---- The backends ----

enum class Kind
{
    Files,
    Jit,
    Aot
};

const char* kind_name(Kind k)
{
    return k == Kind::Files ? "files" : k == Kind::Jit ? "jit" : "aot";
}

bool kind_available(Kind k)
{
    switch (k)
    {
        case Kind::Files:
            return hot::backend_available(hot::BackendKind::ClangFiles);
        case Kind::Jit:
            return hot::backend_available(hot::BackendKind::Jit);
        case Kind::Aot:
            return true;
    }
    return false;
}

std::unique_ptr<hot::Backend> make_backend(Kind k, const Options& o)
{
    hot::DefaultBackendOptions d;
    d.include_dirs = {BENCH_SCRIPT_DIR "/.."};
    d.extra_flags = {o.opt};
    d.extra_flags.insert(d.extra_flags.end(), o.flags.begin(), o.flags.end());
    d.kind = k == Kind::Files ? hot::BackendKind::ClangFiles : hot::BackendKind::Jit;
    return hot::make_default_backend(d);
}

struct Row
{
    double ns = -1; // -1: not measured
    bool mismatch = false;
};

struct BackendResult
{
    Kind kind;
    std::vector<Row> rows; // one for each case
    double reload_ns = -1;
    std::vector<double> chain_old; // ns per call of the reload chain, one for each step
    std::vector<double> chain_new;
    std::string note;
};

// Make an object of a script class and run the case on it.
bool run_instance(const hot::ClassEntry* entry, const Case& c, const Options& o, int64_t native_check, Row& row,
    std::string& note)
{
    if (!entry)
    {
        note = std::string("class ") + c.cls + " not found";
        return false;
    }
    std::unique_ptr<BenchBase> check(static_cast<BenchBase*>(entry->create().native));
    row.mismatch = one_result(*check) != native_check;
    check.reset();
    std::unique_ptr<BenchBase> obj(static_cast<BenchBase*>(entry->create().native));
    row.ns = measure(*obj, c, o);
    return true;
}

// The run-time backends compile one script for each case and load it in a new loader.
void run_cases_compiled(Kind k, const Options& o, const std::vector<int64_t>& checks, const std::vector<int>& selected,
    BackendResult& out)
{
    std::unique_ptr<hot::Backend> backend = make_backend(k, o);
    for (int idx : selected)
    {
        const Case& c = kCases[idx];
        if (!c.script)
            continue; // a native-only row
        const std::string path = std::string(BENCH_SCRIPT_DIR) + "/" + c.script;
        hot::LoadResult r = backend->load({path, read_file(path), {}}, 1);
        if (!r)
        {
            out.note += std::string(c.name) + ": compile failed: " + r.error.substr(0, 300) + "\n";
            continue;
        }
        hot::Loader loader;
        const hot::ReloadResult loaded = loader.load(std::move(*r.module));
        if (!loaded)
        {
            out.note += std::string(c.name) + ": load failed: " + loaded.error + "\n";
            continue;
        }
        std::string note;
        if (!run_instance(loader.find(c.cls), c, o, checks[idx], out.rows[idx], note))
            out.note += std::string(c.name) + ": " + note + "\n";
    }
}

// The AOT scripts are part of this binary. One loader holds all of them.
void run_cases_aot(const Options& o, const std::vector<int64_t>& checks, const std::vector<int>& selected,
    BackendResult& out)
{
    hot::Loader loader;
    const hot::ReloadResult loaded = loader.load_aot(hot::aot_table);
    if (!loaded)
    {
        out.note += "load_aot failed: " + loaded.error + "\n";
        return;
    }
    for (int idx : selected)
    {
        const Case& c = kCases[idx];
        if (!c.script)
            continue; // a native-only row
        std::string note;
        if (!run_instance(loader.find(c.cls), c, o, checks[idx], out.rows[idx], note))
            out.note += std::string(c.name) + ": " + note + "\n";
    }
}

// ---- The reload case ----

constexpr int kReloadFields = 64;
constexpr int kReloadObjects = 100;

// Version 1 has the fields f0..f63. Version 2 adds the field extra, so the state block migrates.
std::string reload_script(bool v2)
{
    std::string s = "using \"BenchEngine.h\";\n\nclass BenchReload : BenchBase {\n";
    for (int i = 0; i < kReloadFields; ++i)
        s += "    int f" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    if (v2)
        s += "    int extra = 7;\n";
    s += "    override long Run(int n) {\n";
    if (v2)
        s += "        extra = extra + 1;\n";
    s += "        long s = 0;\n";
    for (int i = 0; i < kReloadFields; ++i)
        s += "        s += f" + std::to_string(i) + ";\n";
    s += "        return s;\n    }\n}\n";
    return s;
}

// The time of request_reload and commit_pending for kReloadObjects live objects.
void run_reload(Kind k, const Options& o, BackendResult& out)
{
    const int reps = 2; // each rep needs one compile
    std::unique_ptr<hot::Backend> backend = make_backend(k, o);
    const std::string dir = std::string(BENCH_SCRIPT_DIR) + "/";
    std::vector<hot::LoadResult> built; // generation 1, then 2..reps+1 (alternating v2, v1)
    for (int g = 1; g <= reps + 1; ++g)
    {
        const bool v2 = (g % 2) == 0;
        built.push_back(backend->load({dir + (v2 ? "reload_v2.hot" : "reload_v1.hot"), reload_script(v2), {}},
            static_cast<uint32_t>(g)));
        if (!built.back())
        {
            out.note += "reload: compile failed: " + built.back().error.substr(0, 300) + "\n";
            return;
        }
    }
    hot::Loader loader;
    if (const hot::ReloadResult l = loader.load(std::move(*built[0].module)); !l)
    {
        out.note += "reload: load failed: " + l.error + "\n";
        return;
    }
    const hot::ClassEntry* entry = loader.find("BenchReload");
    if (!entry)
    {
        out.note += "reload: class not found\n";
        return;
    }
    std::vector<std::unique_ptr<BenchBase>> objects;
    for (int i = 0; i < kReloadObjects; ++i)
        objects.emplace_back(static_cast<BenchBase*>(entry->create().native));
    std::vector<double> times;
    for (int g = 2; g <= reps + 1; ++g)
    {
        const Clock::time_point t = Clock::now();
        const hot::ReloadResult req = loader.request_reload(std::move(built[g - 1]));
        const hot::ReloadResult com = req ? loader.commit_pending() : req;
        const double ms = ms_since(t);
        if (!com)
        {
            out.note += "reload: failed: " + com.error + "\n";
            return;
        }
        times.push_back(ms * 1e6);
        loader.frame_boundary();
        loader.frame_boundary();
    }
    constexpr int64_t kSum = kReloadFields * (kReloadFields - 1) / 2;
    if (objects.back()->Run(0) != kSum)
        out.note += "reload: a field lost its value in the migration\n";
    out.reload_ns = median(times);
}

// ---- The reload chain: the cost of a call after N reloads ----

// The steps of the chain. The tool measures after 0, 1, 10 and 100 reloads (--reloads limits N).
constexpr int kChainSteps[] = {0, 1, 10, 100};

// Generation g of the chain script. An even generation adds a field, so each reload migrates
// the state block of the object.
std::string chain_script(int g)
{
    std::string s = "using \"BenchEngine.h\";\n\nclass BenchChain : BenchBase {\n    int k = 1;\n";
    if (g % 2 == 0)
        s += "    int pad = 0;\n";
    s += "    override int Hook(int x) {\n        return x + k;\n    }\n\n"
         "    override long Run(int n) {\n        return DriveHook(n);\n    }\n}\n";
    return s;
}

const Case kChainCase = {"chain", "call after N reloads", nullptr, "BenchChain", make_native<NChain>, 5000000, false, 1};

// Loads generation 1, makes an object, then reloads up to the largest step. At each step the
// tool times the call on the old object (made in generation 1 and migrated N times) and on a
// new object (made after the last reload).
void run_chain(Kind k, const Options& o, int64_t native_check, BackendResult& out)
{
    std::unique_ptr<hot::Backend> backend = make_backend(k, o);
    const std::string path = std::string(BENCH_SCRIPT_DIR) + "/chain.hot";
    auto build = [&](int g) { return backend->load({path, chain_script(g), {}}, static_cast<uint32_t>(g)); };
    hot::LoadResult first = build(1);
    if (!first)
    {
        out.note += "chain: compile failed: " + first.error.substr(0, 300) + "\n";
        return;
    }
    hot::Loader loader;
    if (const hot::ReloadResult l = loader.load(std::move(*first.module)); !l)
    {
        out.note += "chain: load failed: " + l.error + "\n";
        return;
    }
    const hot::ClassEntry* entry = loader.find(kChainCase.cls);
    if (!entry)
    {
        out.note += "chain: class not found\n";
        return;
    }
    std::unique_ptr<BenchBase> old_obj(static_cast<BenchBase*>(entry->create().native));
    int done = 0;
    for (int step : kChainSteps)
    {
        if (step > o.reloads)
            break;
        while (done < step)
        {
            ++done;
            hot::LoadResult r = build(done + 1);
            if (!r)
            {
                out.note += "chain: compile failed: " + r.error.substr(0, 300) + "\n";
                return;
            }
            const hot::ReloadResult req = loader.request_reload(std::move(r));
            const hot::ReloadResult com = req ? loader.commit_pending() : req;
            if (!com)
            {
                out.note += "chain: reload failed: " + com.error + "\n";
                return;
            }
            loader.frame_boundary();
            loader.frame_boundary();
        }
        entry = loader.find(kChainCase.cls);
        std::unique_ptr<BenchBase> new_obj(static_cast<BenchBase*>(entry->create().native));
        if (one_result(*old_obj) != native_check || one_result(*new_obj) != native_check)
            out.note += "chain: the result after " + std::to_string(step) + " reloads differs from the native twin\n";
        out.chain_old.push_back(measure(*old_obj, kChainCase, o));
        out.chain_new.push_back(measure(*new_obj, kChainCase, o));
    }
}

// ---- The load section (translate, cold load, warm load) ----

struct LoadResultRow
{
    bool ok = false;
    double miss = 0, hit = 0, cold1 = 0, cold2 = 0, warm1 = 0, warm2 = 0, rss_mb = 0;
    double translate = 0, compile = 0, jit = 0;
};

hot::ScriptSource test_source(const char* file)
{
    const std::string path = std::string(BENCH_SOURCE_DIR) + "/tests/scripts/" + file;
    return {path, read_file(path), {}};
}

std::unique_ptr<hot::Backend> make_explicit(Kind k, bool share, const std::shared_ptr<hotc::TranslatorSession>& session,
    const Options& o)
{
    const std::vector<std::string> includes = split(BENCH_INCLUDES);
#if defined(HOT_WITH_JIT)
    if (k == Kind::Jit)
    {
        hot::JitOptions j;
        j.compiler = BENCH_CXX;
        j.include_dirs = includes;
        j.resource_dir = BENCH_RESOURCE_DIR;
        j.extra_flags = {o.opt};
        j.extra_flags.insert(j.extra_flags.end(), o.flags.begin(), o.flags.end());
        if (share)
            j.session = session;
        return std::make_unique<hot::JitBackend>(std::move(j));
    }
#endif
#if defined(HOT_WITH_CLANG_FILES)
    if (k == Kind::Files)
    {
        hot::ClangFilesOptions f;
        f.compiler = BENCH_CXX;
        f.include_dirs = includes;
        f.resource_dir = BENCH_RESOURCE_DIR;
        f.extra_flags = {o.opt};
        f.extra_flags.insert(f.extra_flags.end(), o.flags.begin(), o.flags.end());
        f.work_dir = std::string(BENCH_WORK_DIR) + "/bench" + std::to_string(getpid());
        std::error_code ec;
        std::filesystem::remove_all(f.work_dir, ec);
        f.remove_work_dir_at_exit = !hot::keep_temp();
        if (share)
            f.session = session;
        return std::make_unique<hot::ClangFilesBackend>(std::move(f));
    }
#endif
    (void)k;
    (void)share;
    (void)session;
    (void)o;
    return nullptr;
}

// One load. Returns the time in ms, or -1 on error (the text goes to stderr).
double timed_load(hot::Backend& b, const hot::ScriptSource& s, uint32_t gen)
{
    const Clock::time_point t = Clock::now();
    hot::LoadResult r = b.load(s, gen);
    const double ms = ms_since(t);
    if (!r)
    {
        std::fprintf(stderr, "load failed: %s\n", r.error.substr(0, 400).c_str());
        return -1;
    }
    // The Loader registers the class. This also proves that the module works.
    hot::Loader loader;
    const hot::ReloadResult loaded = loader.load(std::move(*r.module));
    if (!loaded)
    {
        std::fprintf(stderr, "register failed: %s\n", loaded.error.c_str());
        return -1;
    }
    return ms;
}

LoadResultRow run_load_section(Kind k, const Options& o)
{
    LoadResultRow res;
    const hot::ScriptSource v1 = test_source("player_v1.hot");
    const hot::ScriptSource v2 = test_source("player_v2.hot");
    auto session = std::make_shared<hotc::TranslatorSession>();
    {
        std::unique_ptr<hot::Backend> backend = make_explicit(k, false, session, o);
        if (!backend)
            return res;
        res.cold1 = timed_load(*backend, v1, 1);
        res.cold2 = timed_load(*backend, v2, 2);
        if (res.cold1 < 0 || res.cold2 < 0)
            return res;
    }
    std::vector<double> w1, w2;
    for (int i = 0; i < o.warm; ++i)
    {
        std::unique_ptr<hot::Backend> b = make_explicit(k, !o.no_cache, session, o);
        const double a = timed_load(*b, v1, 1);
        const double c = timed_load(*b, v2, 2);
        if (a < 0 || c < 0)
            return res;
        w1.push_back(a);
        w2.push_back(c);
#if defined(HOT_WITH_JIT)
        if (k == Kind::Jit)
        {
            const hot::JitTimings t = static_cast<hot::JitBackend&>(*b).last_timings();
            res.translate = t.translate_ms, res.compile = t.compile_ms, res.jit = t.jit_ms;
        }
#endif
    }
    res.warm1 = median(w1);
    res.warm2 = median(w2);
    // One translate alone: a new session (miss) and then the same session (hit).
    hotc::TranslateOptions to;
    to.script_text = v1.text;
    to.script_name = v1.name;
    to.generation = 1;
    to.include_dirs = split(BENCH_INCLUDES);
    to.resource_dir = BENCH_RESOURCE_DIR;
    hotc::TranslatorSession ts;
    Clock::time_point t = Clock::now();
    const bool ok1 = hotc::translate(to, ts).ok;
    res.miss = ms_since(t);
    t = Clock::now();
    const bool ok2 = hotc::translate(to, ts).ok;
    res.hit = ms_since(t);
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    res.rss_mb = static_cast<double>(ru.ru_maxrss) / 1024.0;
    res.ok = ok1 && ok2;
    return res;
}

// ---- The output ----

std::string fmt_ns(double ns)
{
    char b[32];
    if (ns < 0)
        return "-";
    std::snprintf(b, sizeof b, ns >= 1000 ? "%.0f" : ns >= 100 ? "%.1f" : "%.2f", ns);
    return b;
}

std::string fmt_ratio(double ns, double native)
{
    char b[32];
    if (ns < 0 || native <= 0)
        return "-";
    std::snprintf(b, sizeof b, "%.2fx", ns / native);
    return b;
}

} // namespace

int main(int argc, char** argv)
{
    // libstdc++ uses plain (not atomic) std::shared_ptr counts while the process has only one
    // thread. The JIT backend starts threads, and a real host has threads. One thread at the
    // start gives every case, native and script, the atomic counts.
    std::thread([] {}).join();
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--backend")
            o.backend = next();
        else if (a == "--filter")
            o.filter = next();
        else if (a == "--iters")
            o.iters = std::atoll(next().c_str());
        else if (a == "--csv")
            o.csv = next();
        else if (a == "--warm")
            o.warm = std::max(1, std::atoi(next().c_str()));
        else if (a == "--reps")
            o.reps = std::max(1, std::atoi(next().c_str()));
        else if (a == "--opt")
            o.opt = next();
        else if (a == "--cxxflag")
            o.flags.push_back(next());
        else if (a == "--no-cache")
            o.no_cache = true;
        else if (a == "--list")
            o.list = true;
        else if (a == "--no-load")
            o.no_load = true;
        else if (a == "--reloads")
            o.reloads = std::max(0, std::atoi(next().c_str()));
        else
        {
            std::fprintf(stderr,
                "usage: bench_backends [--backend all|files|jit|aot] [--filter text] [--iters N] [--list]\n"
                "                      [--csv file] [--warm N] [--reps N] [--opt FLAG] [--cxxflag F]...\n"
                "                      [--no-cache] [--no-load] [--reloads N]\n");
            return 2;
        }
    }
    if (o.backend != "all" && o.backend != "files" && o.backend != "jit" && o.backend != "aot")
    {
        std::fprintf(stderr, "bench_backends: unknown backend '%s'\n", o.backend.c_str());
        return 2;
    }

    std::vector<Kind> kinds;
    std::string notes;
    for (Kind k : {Kind::Files, Kind::Jit, Kind::Aot})
    {
        if (o.backend != "all" && o.backend != kind_name(k))
            continue;
        if (kind_available(k))
            kinds.push_back(k);
        else
            notes += std::string("note: backend '") + kind_name(k) + "' is not built in. Skipped.\n";
    }

    std::vector<int> selected;
    for (int i = 0; i < static_cast<int>(sizeof kCases / sizeof kCases[0]); ++i)
        if (o.filter.empty() || std::string(kCases[i].name).find(o.filter) != std::string::npos)
            selected.push_back(i);

    if (o.list)
    {
        std::printf("backends:\n");
        for (Kind k : {Kind::Files, Kind::Jit, Kind::Aot})
            std::printf("  %-6s %s\n", kind_name(k), kind_available(k) ? "available" : "not built in");
        std::printf("cases:\n");
        for (const Case& c : kCases)
            std::printf("  %-16s %-40s %-18s iters=%lld\n", c.name, c.label, c.script,
                static_cast<long long>(c.iters));
        std::printf("  %-16s %-40s (%d objects x %d fields)\n", "reload", "reload and state migration", kReloadObjects,
            kReloadFields + 1);
        std::printf("  %-16s %-40s (N = 0, 1, 10, 100; --reloads limits N)\n", "chain", kChainCase.label);
        return 0;
    }

    std::printf("bench_backends: HOT_RUNTIME_CHECKS=%d (a build option; the AOT scripts and the native twin use "
                "-O2, script compile flag %s)\n",
        HOT_RUNTIME_CHECKS, o.opt.c_str());

    // The native reference. It also gives the check value of each case.
    std::vector<int64_t> checks(sizeof kCases / sizeof kCases[0], 0);
    std::vector<double> native(checks.size(), -1);
    for (int idx : selected)
    {
        std::unique_ptr<BenchBase> check = kCases[idx].make();
        checks[idx] = one_result(*check);
        std::unique_ptr<BenchBase> obj = kCases[idx].make();
        native[idx] = measure(*obj, kCases[idx], o);
    }
    const bool do_chain = o.filter.empty() || std::string("chain").find(o.filter) != std::string::npos;
    int64_t chain_check = 0;
    double chain_native = -1;
    if (do_chain)
    {
        std::unique_ptr<BenchBase> check = kChainCase.make();
        chain_check = one_result(*check);
        std::unique_ptr<BenchBase> obj = kChainCase.make();
        chain_native = measure(*obj, kChainCase, o);
    }

    // The load section first, so that the first backend has a cold process.
    std::vector<std::pair<Kind, LoadResultRow>> loads;
    const bool do_load = !o.no_load && o.filter.empty();
    std::vector<BackendResult> results;
    for (Kind k : kinds)
    {
        if (do_load && k != Kind::Aot)
            loads.emplace_back(k, run_load_section(k, o));
    }
    for (Kind k : kinds)
    {
        BackendResult br;
        br.kind = k;
        br.rows.assign(checks.size(), Row{});
        const Clock::time_point t = Clock::now();
        try
        {
            if (k == Kind::Aot)
                run_cases_aot(o, checks, selected, br);
            else
            {
                run_cases_compiled(k, o, checks, selected, br);
                if (o.filter.empty() || std::string("reload").find(o.filter) != std::string::npos)
                    run_reload(k, o, br);
                if (do_chain)
                    run_chain(k, o, chain_check, br);
            }
        }
        catch (const std::exception& e)
        {
            br.note += std::string("error: ") + e.what() + "\n";
        }
        std::fprintf(stderr, "backend %s: %.1f s\n", kind_name(k), ms_since(t) / 1000.0);
        results.push_back(std::move(br));
    }

    // Load section output.
    if (!loads.empty())
    {
        std::printf("\nLoad time of tests/scripts/player_v1.hot and player_v2.hot (ms):\n");
        std::printf("%-6s %10s %10s %9s %9s %9s %9s %9s\n", "", "tr.miss", "tr.hit", "cold v1", "cold v2", "warm v1",
            "warm v2", "rss MB");
        for (auto& [k, l] : loads)
        {
            if (!l.ok)
            {
                std::printf("%-6s failed\n", kind_name(k));
                continue;
            }
            std::printf("%-6s %10.1f %10.1f %9.1f %9.1f %9.1f %9.1f %9.0f", kind_name(k), l.miss, l.hit, l.cold1,
                l.cold2, l.warm1, l.warm2, l.rss_mb);
            if (l.compile > 0)
                std::printf("  (last v2: translate=%.1f compile=%.1f jit=%.1f)", l.translate, l.compile, l.jit);
            std::printf("\n");
        }
        std::printf("The cold load of a later backend is warmer than a cold process: the process keeps the LLVM state "
                    "and the file cache.\n");
    }

    // Run-time table.
    std::printf("\nRun time (ns per operation, median of %d runs; ratio = backend / native):\n", o.reps);
    std::printf("%-16s %9s", "case", "native");
    for (const BackendResult& r : results)
        std::printf(" | %9s %7s", kind_name(r.kind), "ratio");
    std::printf("\n");
    std::ostringstream csv;
    csv << "case,native_ns";
    for (const BackendResult& r : results)
        csv << "," << kind_name(r.kind) << "_ns," << kind_name(r.kind) << "_ratio";
    csv << "\n";
    bool any_mismatch = false;
    for (int idx : selected)
    {
        std::printf("%-16s %9s", kCases[idx].name, fmt_ns(native[idx]).c_str());
        csv << kCases[idx].name << "," << native[idx];
        for (const BackendResult& r : results)
        {
            const Row& row = r.rows[idx];
            if (!kCases[idx].script)
            {
                std::printf(" | %9s %7s", "-", "-");
                csv << ",,";
                continue;
            }
            std::string ns = fmt_ns(row.ns);
            if (row.mismatch)
            {
                ns += "!";
                any_mismatch = true;
            }
            std::printf(" | %9s %7s", ns.c_str(), fmt_ratio(row.ns, native[idx]).c_str());
            csv << "," << row.ns << "," << (row.ns > 0 && native[idx] > 0 ? row.ns / native[idx] : -1);
        }
        std::printf("\n");
        csv << "\n";
    }
    const bool show_reload = o.filter.empty() || std::string("reload").find(o.filter) != std::string::npos;
    if (show_reload)
    {
        std::printf("%-16s %9s", "reload", "-");
        csv << "reload,";
        for (const BackendResult& r : results)
        {
            std::printf(" | %9s %7s", fmt_ns(r.reload_ns).c_str(), "-");
            csv << "," << r.reload_ns << ",";
        }
        std::printf("\n");
        csv << "\n";
        std::printf("reload: ns for one request_reload and commit_pending of %d live objects with %d fields (the "
                    "compile is not in it). The AOT backend has no reload.\n",
            kReloadObjects, kReloadFields + 1);
    }
    // The script case "alloc" against each native kind of owner.
    int alloc_idx = -1;
    for (int idx : selected)
        if (std::string(kCases[idx].name) == "alloc")
            alloc_idx = idx;
    if (alloc_idx >= 0)
    {
        std::printf("\nnew and free: the script case 'alloc' against each native owner (ratio = backend / native):\n");
        std::printf("%-17s %9s", "native owner", "native");
        for (const BackendResult& r : results)
            std::printf(" | %7s", kind_name(r.kind));
        std::printf("\n");
        for (int idx : selected)
        {
            const std::string name = kCases[idx].name;
            if (name.compare(0, 5, "alloc") != 0)
                continue;
            std::printf("%-17s %9s", name == "alloc" ? "make_shared" : name.c_str() + 6, fmt_ns(native[idx]).c_str());
            for (const BackendResult& r : results)
                std::printf(" | %7s", fmt_ratio(r.rows[alloc_idx].ns, native[idx]).c_str());
            std::printf("\n");
        }
    }
    // The reload chain.
    bool any_chain = false;
    for (const BackendResult& r : results)
        any_chain = any_chain || !r.chain_old.empty();
    if (any_chain)
    {
        std::printf("\nchain: native to script virtual call after N reloads (ns per call; native %s):\n",
            fmt_ns(chain_native).c_str());
        std::printf("%-6s %5s %9s %7s %9s %7s\n", "", "N", "old obj", "ratio", "new obj", "ratio");
        for (const BackendResult& r : results)
            for (size_t i = 0; i < r.chain_old.size(); ++i)
            {
                std::printf("%-6s %5d %9s %7s %9s %7s\n", kind_name(r.kind), kChainSteps[i],
                    fmt_ns(r.chain_old[i]).c_str(), fmt_ratio(r.chain_old[i], chain_native).c_str(),
                    fmt_ns(r.chain_new[i]).c_str(), fmt_ratio(r.chain_new[i], chain_native).c_str());
                csv << "chain_" << kChainSteps[i] << "_" << kind_name(r.kind) << "," << chain_native << ","
                    << r.chain_old[i] << "," << r.chain_new[i] << "\n";
            }
        std::printf("old obj: made in generation 1 and migrated N times. new obj: made after the last reload.\n");
    }
    if (any_mismatch)
        std::printf("A '!' marks a result that differs from the native twin.\n");
    for (const BackendResult& r : results)
        if (!r.note.empty())
            std::printf("\nnotes for %s:\n%s", kind_name(r.kind), r.note.c_str());
    std::printf("%s", notes.c_str());
    if (!o.csv.empty())
    {
        std::ofstream f(o.csv);
        f << csv.str();
    }
    return 0;
}
