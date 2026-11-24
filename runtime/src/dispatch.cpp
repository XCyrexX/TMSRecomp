// Guest address -> recompiled function, HLE hooks bound by symbol name, traps raised by generated code.
#include "plat.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <shared_mutex>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <thread>
#include <utility>
#include <cstdlib>
#include <map>
#include <set>

#include "rt.h"

extern "C" uint32_t* g_cover = nullptr;

namespace rt {
// the recompiled code and its tables live in game.dll (tools/setup/build_game.py, also run by the CMake build), found
// next to tms.exe: the program download carries no game code, setup builds game.dll from the player's own copy
static const GuestFunc* g_guest_funcs;
static uint32_t g_guest_func_count;
static const GuestHookName* g_hook_names;
static uint32_t g_hook_count;
static const GuestHookName* g_hook_aliases;
static uint32_t g_hook_alias_count;
static const GuestHookName* g_thunks;
static uint32_t g_thunk_count;
static GuestFn* g_hooks;
static GuestFn* g_probes;
const GuestModule* g_modules;
uint32_t g_module_count, g_image_size;

void game_load(const std::filesystem::path& dir) {
#ifdef _WIN32
    auto p = dir / "game.dll";
    HMODULE m = LoadLibraryW(p.c_str());
    if (!m) fatal("cannot load %s (error %lu): run Setup in launcher.exe to build it from your game files",
                  p.string().c_str(), GetLastError());
    auto sym = [&](const char* n) {
        void* a = (void*)GetProcAddress(m, n);
        if (!a) fatal("game.dll has no %s: rebuild it (Setup in launcher.exe)", n);
        return a;
    };
#else
    // game.so: next to tms, or NX_GAME_SO (a build kept outside a read-only install, e.g. an AppImage)
    std::filesystem::path p = getenv("NX_GAME_SO") ? std::filesystem::path(getenv("NX_GAME_SO")) : dir / "game.so";
    void* m = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) fatal("cannot load %s (%s): run the setup (tools/setup/setup.py) to build it from your game files",
                  p.string().c_str(), dlerror());
    auto sym = [&](const char* n) {
        void* a = dlsym(m, n);
        if (!a) fatal("game.so has no %s: rebuild it (tools/setup/setup.py)", n);
        return a;
    };
#endif
    g_guest_funcs = (const GuestFunc*)sym("g_guest_funcs");
    g_guest_func_count = *(const uint32_t*)sym("g_guest_func_count");
    g_hook_names = (const GuestHookName*)sym("g_hook_names");
    g_hook_count = *(const uint32_t*)sym("g_hook_count");
    g_hook_aliases = (const GuestHookName*)sym("g_hook_aliases");
    g_hook_alias_count = *(const uint32_t*)sym("g_hook_alias_count");
    g_thunks = (const GuestHookName*)sym("g_thunks");
    g_thunk_count = *(const uint32_t*)sym("g_thunk_count");
    g_hooks = (GuestFn*)sym("g_hooks");
    g_probes = (GuestFn*)sym("g_probes");
    g_modules = (const GuestModule*)sym("g_modules");
    g_module_count = *(const uint32_t*)sym("g_module_count");
    g_image_size = *(const uint32_t*)sym("g_image_size");
    log("game.dll: %s, %u functions", p.string().c_str(), g_guest_func_count);
}

// probed functions called since the last report, with counts (NX_COVER)
void cover_report() {
    static std::vector<uint32_t> last;
    if (!g_cover) return;
    last.resize(g_hook_count);
    std::string s;
    char b[64];
    for (uint32_t k = 0; k < g_hook_count; k++) {
        uint32_t n = g_cover[k] - last[k];
        last[k] = g_cover[k];
        if (n) { snprintf(b, sizeof b, " %s:%u", g_hook_names[k].name, n); s += b; }
        if (s.size() > 3000) { log("cover:%s", s.c_str()); s.clear(); }  // log lines are cut at 4 KB
    }
    log("cover-end:%s", s.c_str());
}

struct Reg { const char* name; HleFn fn; bool probe; };
static std::vector<Reg>& registry() {
    static std::vector<Reg> r;
    return r;
}
HleReg::HleReg(const char* mangled, HleFn fn, bool probe) { registry().push_back({mangled, fn, probe}); }

// NX_PROBE=name,name,...: log every call of those names-file functions (arguments and caller), up to 64 names
static const char* g_probe_names[64];
template <int K> static void probe_log(Cpu* c) {
    log("probe %s(%08x %08x %08x %08x) lr %s", g_probe_names[K], c->r[0], c->r[1], c->r[2], c->r[3], symbol_at(c->r[14]));
}
template <int... K> static constexpr std::array<HleFn, sizeof...(K)> probe_table(std::integer_sequence<int, K...>) {
    return {probe_log<K>...};
}
static constexpr auto g_probe_loggers = probe_table(std::make_integer_sequence<int, 64>{});

struct TextRange { uint32_t lo, hi; std::vector<GuestFn> fns; };
static std::vector<TextRange> g_text;
static uint32_t g_thunk_lo = 0xFFFFFFFF, g_thunk_hi = 0;
static std::vector<HleFn> g_thunk_fn;
static std::vector<const char*> g_thunk_name;
static std::map<uint32_t, const char*> g_syms;
static std::unordered_map<std::string, uint32_t> g_sym_addr;
constexpr uint32_t THUNK_SIZE = 16;
static std::unordered_map<std::string, uint32_t> g_hook_index;

void dispatch_init() {
    // contiguous runs of functions -> dense tables, split where the gap is large (between modules)
    uint32_t i = 0;
    while (i < g_guest_func_count) {
        uint32_t lo = g_guest_funcs[i].addr, j = i;
        while (j + 1 < g_guest_func_count && g_guest_funcs[j + 1].addr - g_guest_funcs[j].addr < 0x100000) j++;
        TextRange t{lo, g_guest_funcs[j].addr + 4, {}};
        t.fns.assign((t.hi - lo) / 4, nullptr);
        for (uint32_t k = i; k <= j; k++) t.fns[(g_guest_funcs[k].addr - lo) / 4] = g_guest_funcs[k].fn;
        g_text.push_back(std::move(t));
        i = j + 1;
    }
    for (uint32_t k = 0; k < g_hook_count; k++) {
        g_syms[g_hook_names[k].addr] = g_hook_names[k].name;
        g_sym_addr[g_hook_names[k].name] = g_hook_names[k].addr;
    }
    for (uint32_t k = 0; k < g_thunk_count; k++) {
        g_thunk_lo = std::min(g_thunk_lo, g_thunks[k].addr);
        g_thunk_hi = std::max(g_thunk_hi, g_thunks[k].addr + THUNK_SIZE);
    }
    if (g_thunk_count) {
        g_thunk_fn.assign((g_thunk_hi - g_thunk_lo) / THUNK_SIZE, nullptr);
        g_thunk_name.assign(g_thunk_fn.size(), nullptr);
        for (uint32_t k = 0; k < g_thunk_count; k++) g_thunk_name[(g_thunks[k].addr - g_thunk_lo) / THUNK_SIZE] = g_thunks[k].name;
    }
    // bind HLE implementations: exported functions get their hook slot, unresolved imports their thunk
    std::unordered_map<uint32_t, uint32_t> idx_of_addr;
    for (uint32_t k = 0; k < g_hook_count; k++) idx_of_addr.emplace(g_hook_names[k].addr, k);
    std::unordered_map<std::string, uint32_t> hook_idx;  // every alias -> hook slot of its address
    for (uint32_t k = 0; k < g_hook_alias_count; k++) {
        hook_idx.emplace(g_hook_aliases[k].name, idx_of_addr[g_hook_aliases[k].addr]);
        g_sym_addr.emplace(g_hook_aliases[k].name, g_hook_aliases[k].addr);
    }
    g_hook_index = hook_idx;
    int bound = 0, unused = 0;
    for (auto& [name, fn, probe] : registry()) {
        bool ok = false;
        auto h = hook_idx.find(name);
        if (h != hook_idx.end()) { (probe ? g_probes : g_hooks)[h->second] = fn; ok = true; }
        for (size_t k = 0; k < g_thunk_name.size() && !probe; k++)
            if (g_thunk_name[k] && !strcmp(g_thunk_name[k], name)) { g_thunk_fn[k] = fn; ok = true; }
        if (ok) bound++;
        else { unused++; LOGV("HLE %s: no such symbol", name); }
    }
    if (const char* cov = getenv("NX_COVER")) {  // every 10 s (NX_COVER=manual: only on the control line "cover")
        static std::vector<uint32_t> counts(g_hook_count);
        g_cover = counts.data();
        if (strcmp(cov, "manual"))
            std::thread([] { for (;;) { Sleep(10000); cover_report(); } }).detach();
    }
    if (const char* env = getenv("NX_PROBE")) {
        static std::vector<std::string> keep;
        keep.reserve(128);  // g_probe_names points into it
        std::string s = env;
        for (size_t p = 0, n = 0; p <= s.size() && n < 64;) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            keep.push_back(s.substr(p, q - p));
            p = q + 1;
            auto h = hook_idx.find(keep.back());
            if (h == hook_idx.end()) { log("NX_PROBE: no hookable function %s", keep.back().c_str()); continue; }
            g_probe_names[n] = keep.back().c_str();
            g_probes[h->second] = g_probe_loggers[n++];
        }
    }
    log("dispatch: %u functions in %zu ranges, %d HLE bound, %d unmatched", g_guest_func_count, g_text.size(), bound, unused);
}

// call an exported guest function with its HLE hook disabled (to reach the real SDK implementation)
uint32_t call_unhooked(Cpu* c, const char* name, std::initializer_list<uint32_t> args) {
    auto it = g_hook_index.find(name);
    if (it == g_hook_index.end()) fatal("call_unhooked: %s is not hookable", name);
    c->hook_bypass = it->second + 1;  // per thread: HOOK at the function's entry clears it and runs the guest code
    return gcall(c, name, args);
}

GuestFn lookup(uint32_t a) {
    for (auto& t : g_text)
        if (a >= t.lo && a < t.hi) return (a & 3) ? nullptr : t.fns[(a - t.lo) / 4];
    return nullptr;
}

const char* symbol_at(uint32_t a) {
    static thread_local char buf[256];
    auto it = g_syms.upper_bound(a);
    if (it == g_syms.begin()) { snprintf(buf, sizeof buf, "%08x", a); return buf; }
    --it;
    snprintf(buf, sizeof buf, "%08x %s+%#x", a, it->second, a - it->first);
    return buf;
}

uint32_t export_addr(const char* name) {
    auto it = g_sym_addr.find(name);
    return it == g_sym_addr.end() ? 0 : it->second;
}

}  // namespace rt

using namespace rt;

extern "C" void dispatch(Cpu* c, uint32_t t) {
    if (GuestFn f = lookup(t)) { f(c); return; }
    if (t >= g_thunk_lo && t < g_thunk_hi) {
        size_t k = (t - g_thunk_lo) / THUNK_SIZE;
        if (g_thunk_fn[k]) { g_thunk_fn[k](c); return; }
        static std::mutex m;
        static std::set<size_t> seen;
        {
            std::lock_guard<std::mutex> l(m);
            if (seen.insert(k).second) log("unimplemented import %s (lr %s)", g_thunk_name[k], symbol_at(c->r[14]));
        }
        c->r[0] = 0;
        return;
    }
    if (HleFn f = host_callback_at(t)) { f(c); return; }
    guest_bad_branch(c, c->r[14], t);
}

extern "C" void guest_trap(Cpu* c, uint32_t a, const char* why) {
    fatal("guest trap at %s: %s (lr %s)", symbol_at(a), why, symbol_at(c->r[14]));
}

extern "C" void guest_unimpl(Cpu* c, uint32_t a, uint32_t w) {
    fatal("unimplemented instruction %08x at %s (lr %s)", w, symbol_at(a), symbol_at(c->r[14]));
}

extern "C" void guest_bad_branch(Cpu* c, uint32_t from, uint32_t to) {
    fatal("branch to unknown code %08x from %s (sp %08x)", to, symbol_at(from), c->r[13]);
}

extern "C" void guest_noreturn(Cpu* c, uint32_t a) {
    fatal("noreturn call returned at %s", symbol_at(a));
}

extern "C" void guest_svc(Cpu* c, uint32_t imm, uint32_t a) {
    (void)a;
    rt::svc(c, imm);
}

extern "C" void guest_yield(Cpu* c) {
    (void)c;
    SwitchToThread();
}

extern "C" uint32_t cp15_read(Cpu* c, int opc1, int crn, int crm, int opc2) {
    log("cp15 read %d c%d c%d %d at lr %s", opc1, crn, crm, opc2, symbol_at(c->r[14]));
    return 0;
}

extern "C" void cp15_write(Cpu* c, int opc1, int crn, int crm, int opc2, uint32_t v) {
    log("cp15 write %d c%d c%d %d = %08x at lr %s", opc1, crn, crm, opc2, v, symbol_at(c->r[14]));
}

static uint64_t real_counter() {
#ifdef _WIN32
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t q = (uint64_t)now.QuadPart, f = (uint64_t)freq.QuadPart;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t q = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, f = 1000000000ull;
#endif
    return q / f * 19200000 + q % f * 19200000 / f;
}

// guest time = base_guest + (real - base_real) * speed; rebased on every speed change so it never jumps back
#ifdef _WIN32
static SRWLOCK g_speed_lock = SRWLOCK_INIT;
#else
static std::shared_mutex g_speed_lock;
static void AcquireSRWLockExclusive(std::shared_mutex* m) { m->lock(); }
static void ReleaseSRWLockExclusive(std::shared_mutex* m) { m->unlock(); }
static void AcquireSRWLockShared(std::shared_mutex* m) { m->lock_shared(); }
static void ReleaseSRWLockShared(std::shared_mutex* m) { m->unlock_shared(); }
#endif
static uint64_t g_base_real = 0, g_base_guest = 0;
std::atomic<int> rt::g_speed{[] { const char* s = getenv("NX_SPEED"); return s && atoi(s) > 1 ? atoi(s) : 1; }()};

void rt::set_speed(int n) {
    if (n < 1) n = 1;
    AcquireSRWLockExclusive(&g_speed_lock);
    uint64_t real = real_counter();
    g_base_guest += (real - g_base_real) * (uint64_t)g_speed.load();
    g_base_real = real;
    g_speed = n;
    ReleaseSRWLockExclusive(&g_speed_lock);
    log("speed: x%d", n);
}

extern "C" uint64_t guest_counter(void) {
    AcquireSRWLockShared(&g_speed_lock);
    uint64_t t = g_base_guest + (real_counter() - g_base_real) * (uint64_t)g_speed.load();
    ReleaseSRWLockShared(&g_speed_lock);
    return t;
}

// setjmp/longjmp: see lift.py (setjmp is lifted inline at its call sites)
extern "C" GuestJmp* guest_setjmp_slot(Cpu* c, uint32_t buf) {
    (void)c;
    return &cur()->jmps[buf];
}
extern "C" void guest_setjmp_save(Cpu* c, GuestJmp* j) {
    memcpy(j->r, c->r, sizeof j->r);
    memcpy(j->d, &c->vr.x[8], sizeof j->d);
    c->r[0] = 0;
}
extern "C" void guest_setjmp_resume(Cpu* c, GuestJmp* j) {
    memcpy(&c->r[4], &j->r[4], 11 * 4);  // r4-r14
    memcpy(&c->vr.x[8], j->d, sizeof j->d);
    c->r[0] = j->val;
}
HLE_FN(hle_longjmp, "longjmp") {
    auto& m = cur()->jmps;
    auto it = m.find(c->r[0]);
    if (it == m.end()) fatal("longjmp to unknown jmp_buf %08x", c->r[0]);
    it->second.val = c->r[1] ? c->r[1] : 1;
    longjmp(it->second.host, 1);
}
