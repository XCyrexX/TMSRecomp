// NVN objects and command recording. Layouts (offsets in builders and state objects) were read from the SDK's own
// NVN driver functions (private/tms/analysis/nvn_short.txt); see NOTES.md "NVN".
#include "nvn_hle.h"


#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <unordered_set>

using namespace rt;

namespace rt {
std::unordered_map<std::string, HleFn>& nvn_table();
void pace_present(uint32_t interval);  // ui.cpp
}

namespace nvn {

std::mutex g_lock;
void render_drain();  // below: every queued submit / present has run (no-op on the render thread itself)
static std::unordered_map<uint32_t, std::shared_ptr<void>> g_objs;
static std::map<uint32_t, uint32_t> g_pool_by_addr;  // pool storage start -> pool guest object

// Handle lookups run several times per draw (nvnBufferGetAddress / Map = 2 each; profile s72: the lock + hash ~5% of the
// game thread in big areas): each thread keeps a small direct-mapped cache of found objects, dropped when an object is
// destroyed or replaced (a new handle can not be in any cache: only found objects are kept).
static std::atomic<uint64_t> g_obj_gen{1};
template <class T> T* get(uint32_t guest) {
    struct Hit { uint64_t gen = 0; uint32_t guest = 0; void* p = nullptr; };
    thread_local Hit hits[64];
    const uint64_t gen = g_obj_gen.load(std::memory_order_acquire);
    Hit& h = hits[(guest >> 3 ^ guest >> 9) & 63];
    if (h.gen == gen && h.guest == guest) return static_cast<T*>(h.p);
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_objs.find(guest);
    if (it == g_objs.end()) return nullptr;
    h = {gen, guest, it->second.get()};
    return static_cast<T*>(h.p);
}
template <class T> T& make(uint32_t guest) {
    render_drain();  // queued frames look objects up by guest address: they must see the old one
    auto p = std::make_shared<T>();
    T& r = *p;
    std::lock_guard<std::mutex> l(g_lock);
    auto& slot = g_objs[guest];
    if (slot) g_obj_gen.fetch_add(1, std::memory_order_acq_rel);  // replaced: cached pointers to the old one are stale
    slot = p;
    return r;
}
void destroy(uint32_t guest) {
    render_drain();
    std::lock_guard<std::mutex> l(g_lock);
    if (g_objs.erase(guest)) g_obj_gen.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t Buffer::gpu() const {
    Pool* p = get<Pool>(pool);
    return p ? (uint64_t)p->addr + offset : 0;
}

// Pools nest (the game carves small pools out of the storage of a big one): walk back from the nearest start to
// the innermost pool that contains the address.
// The replay asks once per buffer binding (profile s71: lock + map walk ~10% of the game thread in big areas): each thread
// keeps its last 4 answers with the exact address range each holds for, dropped when any pool is created or finalized.
static std::atomic<uint64_t> g_pool_gen{1};
Pool* pool_of(uint64_t gpu, uint32_t* off) {
    struct Hit { uint64_t gen = 0, lo = 0, hi = 0; Pool* p = nullptr; };
    thread_local Hit hits[4];  // draws alternate a few pools (vertex, index, uniform): last 4 answers
    thread_local uint32_t slot = 0;
    const uint64_t gen = g_pool_gen.load(std::memory_order_acquire);
    for (const Hit& h : hits)
        if (h.gen == gen && gpu >= h.lo && gpu < h.hi) {
            if (off) *off = (uint32_t)(gpu - h.p->addr);
            return h.p;
        }
    Hit& hit = hits[slot++ & 3];
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_pool_by_addr.upper_bound((uint32_t)gpu);
    const uint64_t next = it == g_pool_by_addr.end() ? 0x100000000ull : it->first;  // no other pool start below this
    uint64_t lo = 0;  // addresses from lo up take the same walk: the pools skipped below end at or before lo
    bool first = true;
    while (it != g_pool_by_addr.begin()) {
        --it;
        if (first) lo = it->first, first = false;
        auto o = g_objs.find(it->second);
        if (o == g_objs.end()) continue;
        Pool* p = static_cast<Pool*>(o->second.get());
        if (gpu < (uint64_t)p->addr + p->size) {
            if (off) *off = (uint32_t)(gpu - p->addr);
            hit = {gen, lo, std::min<uint64_t>(next, (uint64_t)p->addr + p->size), p};
            return p;
        }
        lo = std::max<uint64_t>(lo, (uint64_t)p->addr + p->size);
    }
    return nullptr;
}

// -------------------------------------------------------------------------------------------- format table
// The driver keeps a 48-byte descriptor per NVNformat; we read it from guest memory (never copied into the repo).
struct FormatInfo { uint32_t bw, bh, bytes, flags; };
static uint32_t g_fmt_table = 0;
FormatInfo format_info(uint32_t f) {
    if (!g_fmt_table) g_fmt_table = ld32(0x2878638);  // GOT slot read by nvnTextureBuilderGetStorageSize
    if (f > 0xA0) return {1, 1, 4, 0};
    uint32_t e = g_fmt_table + 48 * f;
    uint32_t w2 = ld32(e + 8), w4 = ld32(e + 16);
    return {w2 & 0xFF ? w2 & 0xFF : 1, (w2 >> 8) & 0xFF ? (w2 >> 8) & 0xFF : 1, w4 & 0xFF ? w4 & 0xFF : 4, ld32(e)};
}

// GOBs per block in Y before shrinking for small levels: a packaged texture's layout, else 16
uint32_t block_height_max(uint32_t layout) { return (layout & 0xF) ? 1u << std::min(4u, (layout >> 4) & 0xF) : 16u; }

// Block-linear storage size (GOB = 64 B x 8 rows; block height shrinks for small mips), all levels and layers.
static uint32_t storage_size(uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t samples, uint32_t layout) {
    FormatInfo fi = format_info(fmt);
    if (!levels) levels = 1;
    uint64_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
        uint32_t bx = (lw + fi.bw - 1) / fi.bw, by = (lh + fi.bh - 1) / fi.bh;
        uint32_t row = (bx * fi.bytes + 63) & ~63u;
        uint32_t gobs_y = (by + 7) / 8, bh = block_height_max(layout);
        while (bh > 1 && gobs_y <= bh / 2) bh /= 2;
        uint32_t rows = ((gobs_y + bh - 1) / bh) * bh * 8;
        total += (uint64_t)row * rows;
    }
    if (d > 1) {  // each layer starts on a level-0 block boundary (same rule as the upload in vk_texture.cpp)
        uint32_t gobs_y = ((std::max(1u, h) + fi.bh - 1) / fi.bh + 7) / 8, bh = block_height_max(layout);
        while (bh > 1 && gobs_y <= bh / 2) bh /= 2;
        total = (total + 512 * bh - 1) / (512 * bh) * (512 * bh);
    }
    total *= std::max(1u, d) * std::max(1u, samples);
    return (uint32_t)((total + 0x1FF) & ~0x1FFull);
}

}  // namespace nvn

using namespace nvn;

#define NVN(name) \
    static void name(Cpu* c); \
    static int name##_reg = (rt::nvn_table()[#name] = name, 0); \
    static void name(Cpu* c)

static uint64_t u64at(uint32_t lo_reg_val, uint32_t hi_reg_val) { return (uint64_t)hi_reg_val << 32 | lo_reg_val; }
static void ret64(Cpu* c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }

// -------------------------------------------------------------------------------------------- device
NVN(nvnDeviceInitialize) { log("nvn: device %08x", c->r[0]); backend_init(); c->r[0] = 1; }
NVN(nvnDeviceFinalize) {}
NVN(nvnDeviceSetWindowOriginMode) {}
namespace nvn { uint32_t g_depth_mode = 0; }
NVN(nvnDeviceSetDepthMode) { g_depth_mode = c->r[1]; log("nvn: depth mode %u", c->r[1]); }
NVN(nvnDeviceGetWindowOriginMode) { c->r[0] = 1; }
NVN(nvnDeviceGetDepthMode) { c->r[0] = g_depth_mode; }
NVN(nvnDeviceInstallDebugCallback) {}
NVN(nvnDeviceSetIntermediateShaderCache) {}
NVN(nvnDeviceApplyDeferredFinalizes) {}
NVN(nvnDeviceFinalizeCommandHandle) {}
NVN(nvnDeviceIsExternalDebuggerAttached) { c->r[0] = 0; }
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) { ret64(c, guest_counter() * 625 / 12); }  // 19.2 MHz -> ns
NVN(nvnDeviceRegisterFastClearColor) { c->r[0] = 1; }
NVN(nvnDeviceRegisterFastClearColori) { c->r[0] = 1; }
NVN(nvnDeviceRegisterFastClearColorui) { c->r[0] = 1; }
NVN(nvnDeviceRegisterFastClearDepth) { c->r[0] = 1; }

// -------------------------------------------------------------------------------------------- memory pool / buffer
NVN(nvnMemoryPoolInitialize) {  // builder: storage @8, size @0xc, flags @0x10
    uint32_t b = c->r[1];
    Pool& p = make<Pool>(c->r[0]);
    p.addr = ld32(b + 8);
    p.size = ld32(b + 0xC);
    p.flags = ld32(b + 0x10);
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_pool_by_addr[p.addr] = c->r[0];
        g_pool_gen++;
    }
    LOGV("nvn pool %08x: %08x+%x flags %x", c->r[0], p.addr, p.size, p.flags);
    c->r[0] = 1;
}
NVN(nvnMemoryPoolFinalize) {
    render_drain();
    if (Pool* p = get<Pool>(c->r[0])) {
        std::lock_guard<std::mutex> l(g_lock);
        g_pool_by_addr.erase(p->addr);
        g_pool_gen++;
    }
    destroy(c->r[0]);
}
NVN(nvnMemoryPoolMap) { Pool* p = get<Pool>(c->r[0]); c->r[0] = p ? p->addr : 0; }
NVN(nvnMemoryPoolGetBufferAddress) { Pool* p = get<Pool>(c->r[0]); ret64(c, p ? p->addr : 0); }
NVN(nvnMemoryPoolGetSize) { Pool* p = get<Pool>(c->r[0]); c->r[0] = p ? p->size : 0; }
NVN(nvnMemoryPoolGetFlags) { Pool* p = get<Pool>(c->r[0]); c->r[0] = p ? p->flags : 0; }
NVN(nvnMemoryPoolFlushMappedRange) {}
NVN(nvnMemoryPoolInvalidateMappedRange) {}
NVN(nvnMemoryPoolSetDebugLabel) {}

NVN(nvnBufferInitialize) {  // builder: device @4, pool @8, offset @0xc, size @0x10
    uint32_t b = c->r[1];
    Buffer& x = make<Buffer>(c->r[0]);
    x.pool = ld32(b + 8);
    x.offset = ld32(b + 0xC);
    x.size = ld32(b + 0x10);
    c->r[0] = 1;
}
NVN(nvnBufferFinalize) { destroy(c->r[0]); }
NVN(nvnBufferGetAddress) { Buffer* b = get<Buffer>(c->r[0]); ret64(c, b ? b->gpu() : 0); }
NVN(nvnBufferMap) { Buffer* b = get<Buffer>(c->r[0]); c->r[0] = b ? (uint32_t)b->gpu() : 0; }
NVN(nvnBufferGetSize) { Buffer* b = get<Buffer>(c->r[0]); c->r[0] = b ? b->size : 0; }
NVN(nvnBufferGetMemoryPool) { Buffer* b = get<Buffer>(c->r[0]); c->r[0] = b ? b->pool : 0; }
NVN(nvnBufferGetMemoryOffset) { Buffer* b = get<Buffer>(c->r[0]); c->r[0] = b ? b->offset : 0; }
NVN(nvnBufferFlushMappedRange) {}
NVN(nvnBufferInvalidateMappedRange) {}

// -------------------------------------------------------------------------------------------- textures
// texture builder: device @4, pool @8, offset @0xc, flags @0x10, target @0x14, w/h/d @0x18.., levels @0x24,
// format @0x28, samples @0x2c, swizzle @0x30..0x3c, depth-stencil mode @0x40, stride @0x4c
static uint32_t builder_size(uint32_t b) {
    return storage_size(ld32(b + 0x28), ld32(b + 0x18), ld32(b + 0x1C), ld32(b + 0x20), ld32(b + 0x24), ld32(b + 0x2C), ld32(b + 0x48) & 0xFFFF);
}
NVN(nvnTextureBuilderGetStorageSize) { c->r[0] = builder_size(c->r[0]); }
NVN(nvnTextureBuilderGetStorageAlignment) { c->r[0] = 0x200; }
NVN(nvnTextureBuilderGetZCullStorageSize) { c->r[0] = 0; }
NVN(nvnTextureBuilderGetStorageClass) { c->r[0] = 0x100; }
NVN(nvnTextureBuilderGetRawStorageClass) { c->r[0] = 1; }
NVN(nvnTextureInitialize) {
    uint32_t t = c->r[0], b = c->r[1];
    render_drain();
    backend_texture_finalized(get<Texture>(t));  // initialized again without a finalize
    Texture& x = make<Texture>(t);
    x.guest = t;
    x.pool = ld32(b + 8);
    x.offset = ld32(b + 0xC);
    x.flags = ld32(b + 0x10);
    x.target = ld32(b + 0x14);
    x.width = ld32(b + 0x18);
    x.height = ld32(b + 0x1C);
    x.depth = ld32(b + 0x20);
    x.levels = ld32(b + 0x24);
    x.format = ld32(b + 0x28);
    x.samples = ld32(b + 0x2C);
    for (int i = 0; i < 4; i++) x.swizzle[i] = ld32(b + 0x30 + 4 * i);
    x.ds_mode = ld32(b + 0x40);
    x.stride = ld32(b + 0x4C);
    x.layout = ld32(b + 0x48) & 0xFFFF;
    {
        static std::set<uint32_t> seen;  // each distinct packaged layout once
        std::lock_guard<std::mutex> l(g_lock);
        if (seen.insert(x.layout).second) log("nvn: texture layout %#x first seen (%ux%u levels %u fmt %#x)", x.layout, x.width, x.height, x.levels, x.format);
    }
    Pool* p = get<Pool>(x.pool);
    x.gpu = p ? (uint64_t)p->addr + x.offset : 0;
    LOGV("nvn texture %08x: %ux%ux%u fmt %#x levels %u target %u flags %#x", t, x.width, x.height, x.depth, x.format,
         x.levels, x.target, x.flags);
    backend_texture_created(&x);
    c->r[0] = 1;
}
NVN(nvnTextureFinalize) {
    render_drain();
    backend_texture_finalized(get<Texture>(c->r[0]));
    destroy(c->r[0]);
}
#define TEX_GET(name, field) NVN(name) { Texture* t = get<Texture>(c->r[0]); c->r[0] = t ? t->field : 0; }
TEX_GET(nvnTextureGetWidth, width)
TEX_GET(nvnTextureGetHeight, height)
TEX_GET(nvnTextureGetDepth, depth)
TEX_GET(nvnTextureGetLevels, levels)
TEX_GET(nvnTextureGetFormat, format)
TEX_GET(nvnTextureGetTarget, target)
TEX_GET(nvnTextureGetFlags, flags)
TEX_GET(nvnTextureGetSamples, samples)
TEX_GET(nvnTextureGetStride, stride)
TEX_GET(nvnTextureGetMemoryPool, pool)
TEX_GET(nvnTextureGetMemoryOffset, offset)
TEX_GET(nvnTextureGetDepthStencilMode, ds_mode)
NVN(nvnTextureGetStorageSize) {
    Texture* t = get<Texture>(c->r[0]);
    c->r[0] = t ? storage_size(t->format, t->width, t->height, t->depth, t->levels, t->samples, t->layout) : 0;
}
NVN(nvnTextureGetTextureAddress) { Texture* t = get<Texture>(c->r[0]); ret64(c, t ? t->gpu : 0); }
NVN(nvnTextureGetViewOffset) { c->r[0] = 0; }
NVN(nvnTextureFlushTexels) {}
NVN(nvnTextureInvalidateTexels) {}
NVN(nvnTextureSetDebugLabel) {}
NVN(nvnTextureWriteTexels) {  // (tex, view, region, data)
    render_drain();  // queued frames drew with the old texels
    Texture* t = get<Texture>(c->r[0]);
    if (!t) return;
    if (c->r[1]) LOGV("nvn: WriteTexels with a view (level/layer ignored)");
    int32_t rg[6];
    memcpy(rg, GPTR(c->r[2]), 24);
    write_texels(t, rg, (const uint8_t*)GPTR(c->r[3]));
}

// texture / sampler pools: real Initialize keeps (pool, offset, count); registration is ours
struct TexPoolEntry { uint32_t tex = 0; std::vector<uint8_t> view; };
static std::unordered_map<uint32_t, std::unordered_map<uint32_t, TexPoolEntry>> g_tex_pools;
static std::unordered_map<uint32_t, std::unordered_map<uint32_t, Sampler>> g_smp_pools;
NVN(nvnTexturePoolRegisterTexture) {  // (pool, id, texture, view)
    TexPoolEntry e;
    e.tex = c->r[2];
    if (c->r[3]) e.view.assign((uint8_t*)GPTR(c->r[3]), (uint8_t*)GPTR(c->r[3]) + 0x24);
    render_drain();  // queued frames resolve pool ids when they are replayed
    std::lock_guard<std::mutex> l(g_lock);
    g_tex_pools[c->r[0]][c->r[1]] = std::move(e);
}
NVN(nvnTexturePoolRegisterImage) {
    TexPoolEntry e;
    e.tex = c->r[2];
    if (c->r[3]) e.view.assign((uint8_t*)GPTR(c->r[3]), (uint8_t*)GPTR(c->r[3]) + 0x24);
    render_drain();  // queued frames resolve pool ids when they are replayed
    std::lock_guard<std::mutex> l(g_lock);
    g_tex_pools[c->r[0]][c->r[1]] = std::move(e);
}
namespace nvn {
bool pool_texture(uint32_t pool, uint32_t id, Texture** tex, TexView* view) {
    std::lock_guard<std::mutex> l(g_lock);
    auto p = g_tex_pools.find(pool);
    if (p == g_tex_pools.end()) return false;
    auto e = p->second.find(id);
    if (e == p->second.end()) return false;
    if (view) {  // NVNtextureView (layout from the driver's setters): flags @0, swizzle @0xc..0x18,
        // depth-stencil mode @0x1c (0 depth, 1 stencil), target @0x20
        *view = TexView{};
        const auto& v = e->second.view;
        if (v.size() >= 0x24) {
            view->has = true;
            view->flags = v[0];
            memcpy(view->swizzle, &v[0xc], 16);
            memcpy(&view->ds_mode, &v[0x1c], 4);
            memcpy(&view->target, &v[0x20], 4);
        }
    }
    auto o = g_objs.find(e->second.tex);
    *tex = o == g_objs.end() ? nullptr : static_cast<Texture*>(o->second.get());
    return *tex != nullptr;
}
bool pool_sampler(uint32_t pool, uint32_t id, Sampler* s) {
    std::lock_guard<std::mutex> l(g_lock);
    auto p = g_smp_pools.find(pool);
    if (p == g_smp_pools.end()) return false;
    auto e = p->second.find(id);
    if (e == p->second.end()) return false;
    *s = e->second;
    return true;
}
}  // namespace nvn

NVN(nvnSamplerPoolInitialize) {  // (pool object, memory pool, offset, count): keep the same 3 words the driver keeps
    st32(c->r[0], c->r[1]);
    st32(c->r[0] + 4, c->r[2]);
    st32(c->r[0] + 8, c->r[3]);
    c->r[0] = 1;
}
NVN(nvnSamplerPoolRegisterSampler) {  // (pool, id, sampler)
    Sampler* s = get<Sampler>(c->r[2]);
    render_drain();
    std::lock_guard<std::mutex> l(g_lock);
    g_smp_pools[c->r[0]][c->r[1]] = s ? *s : Sampler{};
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {  // (pool, id, builder)
    Sampler s;
    memcpy(s.words, GPTR(c->r[2]), sizeof s.words);
    render_drain();
    std::lock_guard<std::mutex> l(g_lock);
    g_smp_pools[c->r[0]][c->r[1]] = s;
}
NVN(nvnSamplerInitialize) {
    Sampler& s = make<Sampler>(c->r[0]);
    memcpy(s.words, GPTR(c->r[1]), sizeof s.words);
    c->r[0] = 1;
}
NVN(nvnSamplerFinalize) { destroy(c->r[0]); }
NVN(nvnSamplerSetDebugLabel) {}

// -------------------------------------------------------------------------------------------- programs
NVN(nvnProgramSetShaders) {  // (program, count, NVNshaderData[] {u64 data gpu address, u32 control, pad})
    render_drain();
    Program* p = get<Program>(c->r[0]);
    if (!p) p = &make<Program>(c->r[0]);
    uint32_t n = c->r[1], arr = c->r[2];
    p->mask = 0;
    for (uint32_t i = 0; i < n && i < 6; i++) {
        p->stages[i].code = ld64(arr + 16 * i);
        p->stages[i].control = ld32(arr + 16 * i + 8);
        p->mask |= 1u << i;
    }
    p->host = nullptr;
    LOGV("nvn program %08x: %u stages", c->r[0], n);
    static int dumped = 0;
    if (getenv("NX_DUMP_SHADER") && dumped++ < 3)
        for (uint32_t i = 0; i < n && i < 6; i++) {
            uint32_t code = (uint32_t)p->stages[i].code, ctl = p->stages[i].control;
            std::string s1, s2;
            char b[16];
            for (int k = 0; k < 24; k++) { snprintf(b, sizeof b, "%08x ", ld32(code + 4 * k)); s1 += b; }
            for (int k = 0; k < 24; k++) { snprintf(b, sizeof b, "%08x ", ld32(ctl + 4 * k)); s2 += b; }
            log("  stage %u code %08x: %s", i, code, s1.c_str());
            log("  stage %u ctrl %08x: %s", i, ctl, s2.c_str());
        }
    c->r[0] = 1;
}

NVN(nvnProgramSetShadersExt) { nvnProgramSetShaders(c); }  // NVNshaderDataExt: same data/control offsets, stride 16

// -------------------------------------------------------------------------------------------- command buffers
static std::unordered_map<uint32_t, CmdBuf> g_cmdbufs;           // guest command buffer -> recording
// finished handles. On the console a handle points into the command memory it was recorded in and dies when the game
// hands that memory to a command buffer again. Measured: the game re-adds each per-frame buffer's 4 MiB chunk every
// frame, yet still calls state-block handles recorded in a chunk after re-adding it. So only handles that went to a
// queue (per-frame command lists) are freed on chunk reuse; called-only state blocks stay.
static std::unordered_map<uint64_t, std::shared_ptr<const std::vector<Cmd>>> g_handles;
static std::unordered_map<uint64_t, std::vector<uint64_t>> g_chunk_handles;  // memory chunk -> handles recorded in it
static std::unordered_set<uint64_t> g_submitted;                           // handles given to nvnQueueSubmitCommands
static uint64_t g_next_handle = 0x10000;

// Command memory use (s85). The game's per-frame state-block allocator (0x55b8fc / 0x55b9a0: one command buffer per block,
// all given the frame's region) advances its offset by nvnCommandBufferGetCommandMemoryUsed after each recording. That
// returned 0, so every block got the same "chunk" and a handle's lifetime could not be told: handles were never freed
// (Shibuya +150/s standing, +500/s walking, ~2 MB/s of host RAM). Now Used is an estimate (4 bytes per command, below the
// real NVN size, so the game's regions never run out) and each handle owns the bytes [start, end) of its pool that its
// recording used; a later recording over any of those bytes frees it, as the console's overwrite would invalidate it
// (render jobs keep their own reference). NX_CMDMEM_USED=0: the old Used = 0 + free-submitted-on-re-add rule.
static const bool g_cmdmem_used = !getenv("NX_CMDMEM_USED") || atoi(getenv("NX_CMDMEM_USED")) != 0;
static uint32_t cmdmem_used(const CmdBuf& b) { return std::min<uint64_t>(4ull * b.mem_cmds, b.mem_size); }
struct Span { uint32_t end; uint64_t h; };
static const bool g_cmd_debug = getenv("NX_CMD_DEBUG") != nullptr;
struct Kill { uint32_t pool, lo, hi, by_lo, by_hi, by; };
static std::unordered_map<uint64_t, Kill> g_killed;
static std::unordered_map<uint32_t, std::map<uint32_t, Span>> g_pool_spans;  // pool -> start offset -> handle (g_lock)
static void cmdmem_claim(CmdBuf& b, uint64_t h) {
    const uint32_t pool = (uint32_t)(b.chunk >> 32), off = (uint32_t)b.chunk;
    if (!pool) return;  // no command memory given: kept for good (as before)
    const uint32_t lo = off + std::min<uint64_t>(4ull * b.rec_from, b.mem_size), hi = std::max(off + cmdmem_used(b), lo + 1);
    auto& m = g_pool_spans[pool];
    auto it = m.upper_bound(lo);
    if (it != m.begin() && std::prev(it)->second.end > lo) --it;
    while (it != m.end() && it->first < hi) {
        if (g_cmd_debug) g_killed[it->second.h] = {pool, it->first, it->second.end, lo, hi, (uint32_t)h};
        g_handles.erase(it->second.h); it = m.erase(it);
    }
    m[lo] = Span{hi, h};
}

// Finished command lists are recycled (s85, NX_STALL_STATS: ~1000 page faults per Shibuya frame). A big list (> 512 KB:
// Cmd is 72 bytes) is its own VirtualAlloc block in the CRT heap, so every frame's new lists were fresh pages, each first
// touch a page fault, and every free a VirtualFree. Measured 3 A/B pairs: faults per median frame 1050 -> 0, median frame
// 5.9 -> 5.5 ms, 1% low 113-122 -> 120-129 fps. A dead list (last owner: handle map or render job) goes back here with
// its capacity; EndRecording takes the best fit for the next recording of that buffer.
static std::mutex g_cmdvec_lock;
static std::vector<std::vector<Cmd>*> g_cmdvec_free;
static void cmdvec_put(const std::vector<Cmd>* cv) {
    auto* v = const_cast<std::vector<Cmd>*>(cv);
    v->clear();  // blobs released here, on the thread that dropped the list
    std::lock_guard<std::mutex> l(g_cmdvec_lock);
    if (g_cmdvec_free.size() < 64) g_cmdvec_free.push_back(v);
    else delete v;
}
static void cmdvec_take(std::vector<Cmd>& out, size_t n) {  // out is empty: give it a recycled buffer of capacity >= n
    std::vector<Cmd>* best = nullptr;
    {
        std::lock_guard<std::mutex> l(g_cmdvec_lock);
        size_t bi = 0;
        for (size_t i = 0; i < g_cmdvec_free.size(); i++) {
            size_t c = g_cmdvec_free[i]->capacity();
            if (c >= n && (!best || c < best->capacity())) best = g_cmdvec_free[i], bi = i;
        }
        if (best) g_cmdvec_free[bi] = g_cmdvec_free.back(), g_cmdvec_free.pop_back();
    }
    if (!best) { out.reserve(n); return; }
    out.swap(*best);
    delete best;
}

// Every recorded command looks its buffer up (profile s71: the lock was ~10% of the game thread with pool_of): each thread
// keeps the last one; map elements never move, and a Finalize (the only erase) drops every thread's entry.
static std::atomic<uint64_t> g_cmdbuf_gen{1};
static CmdBuf& cb(uint32_t g) {
    struct Last { uint64_t gen = 0; uint32_t guest = 0; CmdBuf* b = nullptr; };
    thread_local Last last;
    const uint64_t gen = g_cmdbuf_gen.load(std::memory_order_acquire);
    if (last.b && last.guest == g && last.gen == gen) return *last.b;
    std::lock_guard<std::mutex> l(g_lock);
    CmdBuf& r = g_cmdbufs[g];
    last = {gen, g, &r};
    return r;
}
static Cmd& rec(Cpu* c, Op op) {
    CmdBuf& b = cb(c->r[0]);
    b.mem_cmds++;
    b.cmds.emplace_back();
    b.cmds.back().op = op;
    return b.cmds.back();
}
static void args(Cmd& k, Cpu* c, int n) {  // r1.. then stack words
    for (int i = 0; i < n && i < 12; i++) k.a[i] = arg(c, i + 1);
}
static void blob(Cmd& k, uint32_t addr, uint32_t size) {
    if (addr) k.blob.assign((uint8_t*)GPTR(addr), (uint8_t*)GPTR(addr) + size);
}

NVN(nvnCommandBufferInitialize) { cb(c->r[0]); c->r[0] = 1; }
NVN(nvnCommandBufferFinalize) { std::lock_guard<std::mutex> l(g_lock); g_cmdbuf_gen++; g_cmdbufs.erase(c->r[0]); }
NVN(nvnCommandBufferAddCommandMemory) {  // (cmd, pool, offset, size)
    uint64_t chunk = (uint64_t)c->r[1] << 32 | c->r[2];
    CmdBuf& b = cb(c->r[0]);
    std::lock_guard<std::mutex> l(g_lock);
    b.chunk = chunk;
    b.mem_size = c->r[3], b.mem_cmds = b.rec_from = 0;
    if (g_cmd_debug) if (static int dn = 0; c->r[2] != 0x4800000 && c->r[2] != 0x4c00000 && dn++ < 60)
        log("nvn: AddCommandMemory cmd %x pool %x off %x size %x lr %s", c->r[0], c->r[1], c->r[2], c->r[3], symbol_at(c->r[14]));
    static uint64_t n = 0;
    // guest MB walks the 4 GB guest reservation with VirtualQuery: ~20 ms under g_lock = a 27 ms hitch every ~6 s (s96), so
    // only with the NX_MEM_REGIONS diagnostic
    static const bool guest_mb = getenv("NX_MEM_REGIONS") != nullptr;
    if (++n % 3000 == 0) log("nvn: %zu live command handles; process private %.0f MB (guest %.0f MB)", g_handles.size(), rt::process_private_mb(), guest_mb ? rt::guest_committed_mb() : -1.0);
    if (g_cmdmem_used) return;  // handles die when a later recording covers their bytes (EndRecording)
    auto it = g_chunk_handles.find(chunk);
    if (it == g_chunk_handles.end()) return;
    auto& hs = it->second;
    hs.erase(std::remove_if(hs.begin(), hs.end(), [](uint64_t h) {
        if (!g_submitted.erase(h)) return false;
        g_handles.erase(h);
        return true;
    }), hs.end());
}
NVN(nvnCommandBufferAddControlMemory) {}
NVN(nvnCommandBufferGetCommandMemorySize) { c->r[0] = g_cmdmem_used ? cb(c->r[0]).mem_size : 0x100000; }
NVN(nvnCommandBufferGetCommandMemoryUsed) { c->r[0] = g_cmdmem_used ? cmdmem_used(cb(c->r[0])) : 0; }
NVN(nvnCommandBufferGetCommandMemoryFree) { CmdBuf& b = cb(c->r[0]); c->r[0] = g_cmdmem_used ? b.mem_size - cmdmem_used(b) : 0x100000; }
NVN(nvnCommandBufferGetControlMemorySize) { c->r[0] = 0x100000; }
NVN(nvnCommandBufferGetControlMemoryUsed) { c->r[0] = 0; }
NVN(nvnCommandBufferGetControlMemoryFree) { c->r[0] = 0x100000; }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { }
NVN(nvnCommandBufferBeginRecording) { CmdBuf& b = cb(c->r[0]); b.cmds.clear(); b.recording = true; b.rec_from = b.mem_cmds; }
NVN(nvnCommandBufferIsRecording) { c->r[0] = cb(c->r[0]).recording; }
NVN(nvnCommandBufferEndRecording) {
    CmdBuf& b = cb(c->r[0]);
    b.recording = false;
    std::lock_guard<std::mutex> l(g_lock);
    const uint64_t h = g_next_handle++;
    const size_t n = b.cmds.size();
    g_handles[h] = std::shared_ptr<const std::vector<Cmd>>(new std::vector<Cmd>(std::move(b.cmds)), cmdvec_put);
    b.mem_cmds++;  // the end-of-list word real NVN writes: even an empty recording uses bytes (its handle stays distinct)
    if (g_cmd_debug) if (static int dn = 0; (uint32_t)b.chunk != 0x4800000 && (uint32_t)b.chunk != 0x4c00000 && dn++ < 60)
        log("nvn: EndRecording cmd %x -> handle %llx, chunk %llx, commands %u..%u (lr %s)", c->r[0], (unsigned long long)h, (unsigned long long)b.chunk, b.rec_from, b.mem_cmds, symbol_at(c->r[14]));
    if (g_cmdmem_used) cmdmem_claim(b, h);
    else g_chunk_handles[b.chunk].push_back(h);
    b.cmds.clear();
    cmdvec_take(b.cmds, n);  // the next recording of this buffer is usually as long
    ret64(c, h);
}
NVN(nvnCommandBufferSetDebugLabel) {}
NVN(nvnCommandBufferInsertDebugMarker) {}
NVN(nvnCommandBufferInsertDebugMarkerStatic) {}
NVN(nvnCommandBufferInsertDebugMarkerDynamic) {}
NVN(nvnCommandBufferPushDebugGroup) {}
NVN(nvnCommandBufferPushDebugGroupStatic) {}
NVN(nvnCommandBufferPushDebugGroupDynamic) {}
NVN(nvnCommandBufferPopDebugGroup) {}
NVN(nvnCommandBufferPopDebugGroupId) {}

NVN(nvnCommandBufferSetRenderTargets) {  // (cmd, n, NVNtexture** colors, NVNtextureView** views, depth*, depthView*)
    Cmd& k = rec(c, OP_RENDER_TARGETS);
    uint32_t n = c->r[1], cols = c->r[2], views = c->r[3], dep = arg(c, 4), dview = arg(c, 5);
    k.a[0] = n;
    for (uint32_t i = 0; i < n && i < 8; i++) k.a[1 + i] = cols ? ld32(cols + 4 * i) : 0;
    k.a[9] = dep;
    // views (0x24 bytes each) after the colours, then the depth view
    k.blob.resize(9 * 0x24);
    for (uint32_t i = 0; i < n && i < 8; i++)
        if (views && ld32(views + 4 * i)) memcpy(&k.blob[i * 0x24], GPTR(ld32(views + 4 * i)), 0x24);
    if (dview) memcpy(&k.blob[8 * 0x24], GPTR(dview), 0x24);
}
NVN(nvnCommandBufferSetViewport) { args(rec(c, OP_VIEWPORT), c, 4); }
NVN(nvnCommandBufferSetViewports) { Cmd& k = rec(c, OP_VIEWPORT); k.a[0] = 0xFFFFFFFF; k.a[1] = c->r[1]; k.a[2] = c->r[2]; blob(k, c->r[3], 16 * c->r[2]); }
NVN(nvnCommandBufferSetScissor) { args(rec(c, OP_SCISSOR), c, 4); }
NVN(nvnCommandBufferSetScissors) { Cmd& k = rec(c, OP_SCISSOR); k.a[0] = 0xFFFFFFFF; k.a[1] = c->r[1]; k.a[2] = c->r[2]; blob(k, c->r[3], 16 * c->r[2]); }
NVN(nvnCommandBufferSetDepthRange) { Cmd& k = rec(c, OP_DEPTH_RANGE); memcpy(&k.a[0], &c->vr.f[0], 8); }
NVN(nvnCommandBufferClearColor) { Cmd& k = rec(c, OP_CLEAR_COLOR); k.a[0] = c->r[1]; k.a[1] = c->r[3]; blob(k, c->r[2], 16); }
NVN(nvnCommandBufferClearColori) { Cmd& k = rec(c, OP_CLEAR_COLOR); k.a[0] = c->r[1]; k.a[1] = c->r[3]; k.a[2] = 1; blob(k, c->r[2], 16); }
NVN(nvnCommandBufferClearColorui) { Cmd& k = rec(c, OP_CLEAR_COLOR); k.a[0] = c->r[1]; k.a[1] = c->r[3]; k.a[2] = 2; blob(k, c->r[2], 16); }
NVN(nvnCommandBufferClearDepthStencil) {  // (cmd, float depth s0, depthMask r1, stencil r2, stencilMask r3)
    Cmd& k = rec(c, OP_CLEAR_DS);
    memcpy(&k.a[0], &c->vr.f[0], 4);
    k.a[1] = c->r[1];
    k.a[2] = c->r[2];
    k.a[3] = c->r[3];
}
NVN(nvnCommandBufferBindProgram) { args(rec(c, OP_PROGRAM), c, 2); }
NVN(nvnCommandBufferBindBlendState) { blob(rec(c, OP_BLEND), c->r[1], 8); }
NVN(nvnCommandBufferBindColorState) { blob(rec(c, OP_COLOR), c->r[1], 4); }
NVN(nvnCommandBufferBindChannelMaskState) { blob(rec(c, OP_CHANNEL_MASK), c->r[1], 4); }
NVN(nvnCommandBufferBindDepthStencilState) { blob(rec(c, OP_DEPTH_STENCIL), c->r[1], 8); }
NVN(nvnCommandBufferBindMultisampleState) { blob(rec(c, OP_MULTISAMPLE), c->r[1], 0x18); }
NVN(nvnCommandBufferBindPolygonState) { blob(rec(c, OP_POLYGON), c->r[1], 4); }
NVN(nvnCommandBufferBindVertexAttribState) { Cmd& k = rec(c, OP_VTX_ATTRIB); k.a[0] = c->r[1]; blob(k, c->r[2], 4 * c->r[1]); }
NVN(nvnCommandBufferBindVertexStreamState) { Cmd& k = rec(c, OP_VTX_STREAM); k.a[0] = c->r[1]; blob(k, c->r[2], 8 * c->r[1]); }
NVN(nvnCommandBufferBindVertexBuffer) {  // (cmd, index, addr64 r2:r3, size64 [sp])
    Cmd& k = rec(c, OP_VTX_BUFFER);
    k.a[0] = c->r[1]; k.a[1] = c->r[2]; k.a[2] = c->r[3]; k.a[3] = arg(c, 4); k.a[4] = arg(c, 5);
}
NVN(nvnCommandBufferBindVertexBuffers) {  // (cmd, first, count, NVNbufferRange[] {u64 addr, u64 size})
    Cmd& k = rec(c, OP_VTX_BUFFER);
    k.a[0] = 0xFFFFFFFF; k.a[1] = c->r[1]; k.a[2] = c->r[2];
    blob(k, c->r[3], 16 * c->r[2]);
}
NVN(nvnCommandBufferBindUniformBuffer) {  // (cmd, stage, index, addr64 [sp], size64 [sp+8])
    Cmd& k = rec(c, OP_UNIFORM);
    k.a[0] = c->r[1]; k.a[1] = c->r[2]; k.a[2] = arg(c, 4); k.a[3] = arg(c, 5); k.a[4] = arg(c, 6); k.a[5] = arg(c, 7);
}
NVN(nvnCommandBufferBindUniformBuffers) {  // (cmd, stage, first, count, ranges [sp])
    Cmd& k = rec(c, OP_UNIFORM);
    k.a[0] = c->r[1]; k.a[1] = 0xFFFFFFFF; k.a[2] = c->r[2]; k.a[3] = c->r[3];
    blob(k, arg(c, 4), 16 * c->r[3]);
}
NVN(nvnCommandBufferBindStorageBuffer) {
    Cmd& k = rec(c, OP_STORAGE);
    k.a[0] = c->r[1]; k.a[1] = c->r[2]; k.a[2] = arg(c, 4); k.a[3] = arg(c, 5); k.a[4] = arg(c, 6); k.a[5] = arg(c, 7);
}
NVN(nvnCommandBufferBindTexture) {  // (cmd, stage, index, handle64 [sp])
    Cmd& k = rec(c, OP_TEXTURE);
    k.a[0] = c->r[1]; k.a[1] = c->r[2]; k.a[2] = arg(c, 4); k.a[3] = arg(c, 5);
}
NVN(nvnCommandBufferBindTextures) {  // (cmd, stage, first, count, handles [sp])
    Cmd& k = rec(c, OP_TEXTURE);
    k.a[0] = c->r[1]; k.a[1] = 0xFFFFFFFF; k.a[2] = c->r[2]; k.a[3] = c->r[3];
    blob(k, arg(c, 4), 8 * c->r[3]);
}
NVN(nvnCommandBufferBindImage) {
    Cmd& k = rec(c, OP_IMAGE);
    k.a[0] = c->r[1]; k.a[1] = c->r[2]; k.a[2] = arg(c, 4); k.a[3] = arg(c, 5);
}
NVN(nvnCommandBufferSetTexturePool) { Cmd& k = rec(c, OP_TEX_POOL); k.a[0] = c->r[1]; blob(k, c->r[1], 12); }
NVN(nvnCommandBufferSetSamplerPool) { Cmd& k = rec(c, OP_SMP_POOL); k.a[0] = c->r[1]; blob(k, c->r[1], 12); }
NVN(nvnCommandBufferDrawArrays) { args(rec(c, OP_DRAW), c, 3); }
NVN(nvnCommandBufferDrawElements) { args(rec(c, OP_DRAW_INDEXED), c, 5); }  // mode, type, count, addr64
NVN(nvnCommandBufferDrawElementsBaseVertex) { args(rec(c, OP_DRAW_INDEXED), c, 6); }
NVN(nvnCommandBufferDrawArraysInstanced) { args(rec(c, OP_DRAW_INSTANCED), c, 5); }
NVN(nvnCommandBufferDrawElementsInstanced) { args(rec(c, OP_DRAW_INDEXED_INSTANCED), c, 8); }
NVN(nvnCommandBufferDispatchCompute) { args(rec(c, OP_DISPATCH), c, 3); }
NVN(nvnCommandBufferBarrier) { args(rec(c, OP_BARRIER), c, 1); }
NVN(nvnCommandBufferSetBlendColor) { blob(rec(c, OP_BLEND_COLOR), c->r[1], 16); }
NVN(nvnCommandBufferSetLineWidth) { Cmd& k = rec(c, OP_LINE_WIDTH); memcpy(&k.a[0], &c->vr.f[0], 4); }
NVN(nvnCommandBufferSetPointSize) { Cmd& k = rec(c, OP_POINT_SIZE); memcpy(&k.a[0], &c->vr.f[0], 4); }
NVN(nvnCommandBufferSetStencilRef) { args(rec(c, OP_STENCIL_REF), c, 2); }
// The masks are not the reference (they overwrote it). Pipelines keep masks 0xFF (dynamic masks would change every
// pipeline's driver-cache hash); shortcut: any other mask only logs, make them dynamic if a log line ever appears.
static void stencil_mask_note(const char* what, uint32_t v) {
    static uint32_t seen = 0xFF;
    if ((v & 0xFF) != 0xFF && v != seen) seen = v, log("nvn: stencil %s mask %08x ignored", what, v);
}
NVN(nvnCommandBufferSetStencilMask) { stencil_mask_note("write", c->r[2]); }
NVN(nvnCommandBufferSetStencilValueMask) { stencil_mask_note("value", c->r[2]); }
NVN(nvnCommandBufferSetPrimitiveRestart) { args(rec(c, OP_PRIM_RESTART), c, 2); }
NVN(nvnCommandBufferSetAlphaRef) { Cmd& k = rec(c, OP_ALPHA_REF); memcpy(&k.a[0], &c->vr.f[0], 4); }
NVN(nvnCommandBufferSetSampleMask) {}
NVN(nvnCommandBufferSetDepthClamp) {}
NVN(nvnCommandBufferSetDepthBounds) {}
NVN(nvnCommandBufferSetRasterizerDiscard) {}
NVN(nvnCommandBufferSetPolygonOffsetClamp) {}
NVN(nvnCommandBufferSetShaderScratchMemory) {}
NVN(nvnCommandBufferSetTiledCacheAction) {}
NVN(nvnCommandBufferSetTiledCacheTileSize) {}
NVN(nvnCommandBufferSetCopyRowStride) { args(rec(c, OP_COPY_BUF_TEX), c, 1); CmdBuf& b = cb(c->r[0]); b.cmds.back().op = OP_NONE; }
NVN(nvnCommandBufferSetConservativeRasterEnable) {}
NVN(nvnCommandBufferSetConservativeRasterDilate) {}
// (cmd, texture, view, NVNcopyRegion*, colour[4] [sp], channel mask [sp+4]); the i/ui variants only differ in the
// colour's type, which the clear passes through as raw bits
static void clear_texture(Cpu* c) {
    Cmd& k = rec(c, OP_CLEAR_TEX);
    k.a[0] = c->r[1];
    k.a[1] = arg(c, 5);
    k.blob.resize(40);
    memcpy(&k.blob[0], GPTR(c->r[3]), 24);
    memcpy(&k.blob[24], GPTR(arg(c, 4)), 16);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c); }
NVN(nvnCommandBufferDiscardColor) {}
NVN(nvnCommandBufferDiscardDepthStencil) {}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src addr64 r2:r3, dstTex [sp], dstView [sp+4], region* [sp+8], flags [sp+12])
    Cmd& k = rec(c, OP_COPY_BUF_TEX);
    k.a[0] = c->r[2]; k.a[1] = c->r[3]; k.a[2] = arg(c, 4); k.a[3] = arg(c, 5); k.a[5] = arg(c, 7);
    blob(k, arg(c, 6), 24);  // NVNcopyRegion: x, y, z, w, h, d
    if (k.a[3]) { k.blob.resize(24 + 0x24); memcpy(&k.blob[24], GPTR(k.a[3]), 0x24); }
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView [sp], dstRegion [sp+4], flags [sp+8])
    Cmd& k = rec(c, OP_COPY_TEX_TEX);
    k.a[0] = c->r[1]; k.a[2] = arg(c, 4); k.a[4] = arg(c, 6); k.a[5] = arg(c, 7);
    k.blob.resize(48);
    memcpy(&k.blob[0], GPTR(c->r[3]), 24);
    memcpy(&k.blob[24], GPTR(arg(c, 6)), 24);
}
NVN(nvnCommandBufferCopyBufferToBuffer) { args(rec(c, OP_COPY_BUF_BUF), c, 7); }
NVN(nvnCommandBufferWaitSync) {}
// (cmd, count, handles*): a finished handle never changes, so calling it = copying its commands in here.
// (s71 tried an OP_CALL that replays the called list in place instead: 4% slower, the many small lists cost more than
// the copies; Blob copies share their bytes, s69.)
NVN(nvnCommandBufferCallCommands) {
    CmdBuf& b = cb(c->r[0]);
    b.mem_cmds++;
    std::lock_guard<std::mutex> l(g_lock);
    for (uint32_t i = 0; i < c->r[1]; i++) {
        auto it = g_handles.find(ld64(c->r[2] + 8 * i));
        if (it != g_handles.end()) b.cmds.insert(b.cmds.end(), it->second->begin(), it->second->end());
        else if (static int un = 0; un++ < 30) {
            auto k = g_killed.find(ld64(c->r[2] + 8 * i));
            if (k == g_killed.end()) log("nvn: CallCommands with unknown handle %llx (lr %s)", (unsigned long long)ld64(c->r[2] + 8 * i), symbol_at(c->r[14]));
            else log("nvn: CallCommands with unknown handle %llx (lr %s): pool %x [%x, %x) freed by %x [%x, %x)", (unsigned long long)ld64(c->r[2] + 8 * i), symbol_at(c->r[14]),
                     k->second.pool, k->second.lo, k->second.hi, k->second.by, k->second.by_lo, k->second.by_hi);
        }
    }
}
NVN(nvnCommandBufferCopyCommands) { nvnCommandBufferCallCommands(c); }
NVN(nvnCommandBufferUpdateUniformBuffer) {  // (cmd, addr64 r2:r3, size64 [sp], offset64 [sp+8], size [sp+16], data [sp+20])
    Cmd& k = rec(c, OP_INLINE_UNIFORM);
    k.a[0] = c->r[2]; k.a[1] = c->r[3]; k.a[2] = arg(c, 6); k.a[3] = arg(c, 8);
    blob(k, arg(c, 9), arg(c, 8));
}

// -------------------------------------------------------------------------------------------- sync
// Syncs hold the GPU serial (vk.cpp submit timeline) that signals them; 0 = signaled. Submits run asynchronously, so
// the game's waits (before reusing uniform/vertex memory or reading GPU results) must really wait.
// job: the render-thread job that will set the serial (0 = none; JOB_ANY = a recorded FenceSync not replayed yet).
static const uint64_t JOB_ANY = ~0ull;
struct SyncObj { std::atomic<uint64_t> serial{0}, job{0}; };
void nvn::sync_mark(uint32_t sync, uint64_t serial) {  // vk_replay.cpp: CommandBufferFenceSync in a submit with this serial
    if (SyncObj* s = get<SyncObj>(sync)) s->serial = serial, s->job = 0;
}

// -------------------------------------------------------------------------------------------- render thread
// Draw replay (backend_submit) was 35-40% of the game thread in big areas (s71-s80 profiles). With a variable frame rate
// (fps 0 or > 60) submits, queue fences and presents go to one render thread in order, and the game thread starts the next
// frame's logic meanwhile. The game allows this itself: before each submit it waits for the sync it fenced two frames
// earlier (measured s81: 2 nvnSyncWait, 1 submit, 1 nvnQueueFenceSync, 1 present per frame, in that order), so its
// per-frame memory is double-buffered. Rules that keep the old order of effects:
//  - a sync whose fence is still queued is not signaled: nvnSyncWait first waits for that job;
//  - anything that changes what a queued frame looks up by guest address (object create / destroy, pool registration,
//    program shaders, WriteTexels, QueueFinish) first waits until the queue is empty (render_drain);
//  - the game is at most one frame ahead: a present waits until the previous present has run;
//  - the UI clock a frame's sprites are smoothed with is the one at its submit, not at its replay.
// At fps 30 / 60 (vsync-paced presents) and with NX_ASYNC_REPLAY=0 every job runs at once on the calling thread, as before.
namespace vk { bool gpu_is_amd(); }  // vk.cpp
namespace nvn {
uint32_t g_replay_ui_tick = 0;  // vk_replay.cpp ui_sprite_smooth: the UI clock of the frame being replayed
float g_replay_ui_frac = 1;
struct Job {
    enum Kind { SUBMIT, FENCE, PRESENT } kind;
    std::vector<std::shared_ptr<const std::vector<Cmd>>> keep;  // SUBMIT: the lists, alive until replayed
    uint32_t sync = 0;       // FENCE: guest sync object
    Texture* tex = nullptr;  // PRESENT
    uint32_t ui_tick = 0;
    float ui_frac = 1;
};
static std::mutex g_rq_lock;
static std::condition_variable g_rq_cv, g_rq_done_cv;
static std::deque<Job> g_rq;
static std::atomic<uint64_t> g_rq_enq{0}, g_rq_done{0};  // jobs queued / finished (job id = its g_rq_enq value)
static uint64_t g_rq_pres_enq = 0, g_rq_pres_done = 0;   // g_rq_lock
static thread_local bool t_render = false;

static void job_run(Job& j) {
    if (j.kind == Job::SUBMIT) {
        g_replay_ui_tick = j.ui_tick, g_replay_ui_frac = j.ui_frac;
        std::vector<const std::vector<Cmd>*> lists;
        for (auto& k : j.keep) lists.push_back(k.get());
        backend_submit(lists);
    } else if (j.kind == Job::FENCE) {
        if (SyncObj* s = get<SyncObj>(j.sync)) s->serial = vk::gpu_last_submitted();
    } else backend_present(j.tex);
}
static bool render_async() {
    // NX_ASYNC_REPLAY=0: never, 2: also at 30 / 60. AMD GPUs use it at every fps: on a ROG Ally (Z1 Extreme, RADV) the
    // game thread needs ~10-15 ms per frame for the game itself, replay on top of it missed 60 fps.
    static const int mode = getenv("NX_ASYNC_REPLAY") ? atoi(getenv("NX_ASYNC_REPLAY")) : vk::gpu_is_amd() ? 2 : 1;
    const int fps = fps_setting();
    return mode != 0 && (mode == 2 || fps == 0 || fps > 60);
}
void render_drain() {
    if (t_render) return;
    const uint64_t target = g_rq_enq.load(std::memory_order_acquire);
    if (g_rq_done.load(std::memory_order_acquire) >= target) return;
    vk::RenderWait rw;
    std::unique_lock<std::mutex> l(g_rq_lock);
    g_rq_done_cv.wait(l, [&] { return g_rq_done.load() >= target; });
}
static bool job_wait(uint64_t job, uint64_t timeout_ns) {  // false: not done within the timeout
    if (t_render || g_rq_done.load(std::memory_order_acquire) >= job) return true;
    if (!timeout_ns) return false;
    vk::RenderWait rw;
    std::unique_lock<std::mutex> l(g_rq_lock);
    auto done = [&] { return g_rq_done.load() >= job; };
    if (timeout_ns >= 60000000000ull) { g_rq_done_cv.wait(l, done); return true; }
    return g_rq_done_cv.wait_for(l, std::chrono::nanoseconds(timeout_ns), done);
}
static uint64_t job_queue(Job&& j) {  // returns the job id (0: ran at once)
    if (t_render || !render_async()) {
        render_drain();  // the mode can change while jobs are queued (control "fps N")
        job_run(j);
        return 0;
    }
    static bool started = false;
    std::unique_lock<std::mutex> l(g_rq_lock);
    if (!started) {
        started = true;
        log("nvn: draw replay on a render thread (NX_ASYNC_REPLAY=0: on the game thread)");
        std::thread([] {
            t_render = true;
            std::unique_lock<std::mutex> l(g_rq_lock);
            for (;;) {
                g_rq_cv.wait(l, [] { return !g_rq.empty(); });
                Job j = std::move(g_rq.front());
                g_rq.pop_front();
                l.unlock();
                job_run(j);
                j.keep.clear();  // frees the lists outside the lock
                l.lock();
                if (j.kind == Job::PRESENT) g_rq_pres_done++;
                g_rq_done.fetch_add(1, std::memory_order_release);
                g_rq_done_cv.notify_all();
            }
        }).detach();
    }
    if (j.kind == Job::PRESENT) {  // at most one frame ahead: the previous present has run
        vk::RenderWait rw;
        g_rq_done_cv.wait(l, [] { return g_rq_pres_done >= g_rq_pres_enq; });
        g_rq_pres_enq++;
    }
    g_rq.push_back(std::move(j));
    const uint64_t id = g_rq_enq.fetch_add(1, std::memory_order_release) + 1;
    g_rq_cv.notify_one();
    return id;
}
}  // namespace nvn
NVN(nvnCommandBufferFenceSync) {  // (cmd, sync, condition, flags)
    args(rec(c, OP_SYNC), c, 3);
    if (SyncObj* s = get<SyncObj>(c->r[1])) s->job = JOB_ANY;  // until the list is replayed (sync_mark): waits drain the queue first
}
NVN(nvnSyncInitialize) { make<SyncObj>(c->r[0]); c->r[0] = 1; }
NVN(nvnSyncFinalize) { destroy(c->r[0]); }
NVN(nvnSyncSetDebugLabel) {}
NVN(nvnSyncWait) {  // (sync, timeout ns r2:r3) -> 0 already signaled, 1 condition satisfied, 2 timeout expired
    SyncObj* s = get<SyncObj>(c->r[0]);
    const uint64_t timeout = (uint64_t)c->r[3] << 32 | c->r[2];
    if (const uint64_t job = s ? s->job.load() : 0) {  // its fence is still queued for the render thread
        if (job == JOB_ANY) render_drain();
        else if (!job_wait(job, timeout)) { c->r[0] = 2; return; }
    }
    uint64_t serial = s ? s->serial.load() : 0;
    if (!serial || vk::gpu_wait(serial, 0)) { c->r[0] = 0; return; }
    c->r[0] = vk::gpu_wait(serial, timeout) ? 1 : 2;
}
NVN(nvnQueueFenceSync) {  // (queue, sync, condition, flags): signaled when the work submitted so far is done
    SyncObj* s = get<SyncObj>(c->r[1]);
    if (!s) return;
    Job j{Job::FENCE};
    j.sync = c->r[1];
    s->job = job_queue(std::move(j));
}
NVN(nvnQueueWaitSync) { c->r[0] = 1; }  // one Vulkan queue: GPU order already holds
NVN(nvnEventInitialize) { c->r[0] = 1; }
NVN(nvnEventFinalize) {}
NVN(nvnEventGetValue) { c->r[0] = 0; }
NVN(nvnEventSignal) {}

// -------------------------------------------------------------------------------------------- queue / window
NVN(nvnQueueInitialize) { make<Queue>(c->r[0]); c->r[0] = 1; }
NVN(nvnQueueFinalize) { destroy(c->r[0]); }
NVN(nvnQueueSetDebugLabel) {}
NVN(nvnQueueGetError) { c->r[0] = 0; }
NVN(nvnQueueGetTotalCommandMemoryUsed) { c->r[0] = 0; }
NVN(nvnQueueGetTotalControlMemoryUsed) { c->r[0] = 0; }
NVN(nvnQueueGetTotalComputeMemoryUsed) { c->r[0] = 0; }
NVN(nvnQueueResetMemoryUsageCounts) {}
NVN(nvnQueueSubmitCommands) {  // (queue, count, handles*)
    Job j{Job::SUBMIT};  // keep: alive through the submit even if freed meanwhile
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (uint32_t i = 0; i < c->r[1]; i++) {
            auto it = g_handles.find(ld64(c->r[2] + 8 * i));
            if (it != g_handles.end()) { j.keep.push_back(it->second); if (!g_cmdmem_used) g_submitted.insert(it->first); }
            else if (static int n = 0; n++ < 20) log("nvn: SubmitCommands with unknown handle %llx", (unsigned long long)ld64(c->r[2] + 8 * i));
        }
    }
    ui_clock(&j.ui_tick, &j.ui_frac);
    job_queue(std::move(j));
}
NVN(nvnQueueFlush) {}
NVN(nvnQueueFinish) { render_drain(); vk::gpu_wait(vk::gpu_last_submitted(), UINT64_MAX); }

namespace nvn { std::unordered_set<uint32_t> g_window_tex; }
NVN(nvnWindowInitialize) {  // builder: numTextures @8, textures** @0x10, nativeWindow @0x14, interval-1 @0x18
    Window& w = make<Window>(c->r[0]);
    uint32_t b = c->r[1];
    w.ntex = std::min<uint32_t>(4, ld32(b + 8));
    for (uint32_t i = 0; i < w.ntex; i++) w.textures[i] = ld32(ld32(b + 0x10) + 4 * i), nvn::g_window_tex.insert(w.textures[i]);
    w.interval = ld32(b + 0x18) + 1;
    log("nvn: window with %u textures (%08x %08x), present interval %u", w.ntex, w.textures[0], w.textures[1], w.interval);
    c->r[0] = 1;
}
NVN(nvnWindowFinalize) { destroy(c->r[0]); }
NVN(nvnWindowGetNumTextures) { Window* w = get<Window>(c->r[0]); c->r[0] = w ? w->ntex : 0; }
NVN(nvnWindowGetNumActiveTextures) { Window* w = get<Window>(c->r[0]); c->r[0] = w ? w->ntex : 0; }
NVN(nvnWindowSetNumActiveTextures) {}
NVN(nvnWindowGetPresentInterval) { Window* w = get<Window>(c->r[0]); c->r[0] = w ? w->interval : 1; }
NVN(nvnWindowSetPresentInterval) {
    Window* w = get<Window>(c->r[0]);
    if (w && w->interval != c->r[1]) log("nvn: present interval %u (lr %s)", c->r[1], symbol_at(c->r[14]));  // lr: the game's frame-rate code (milestone 4)
    if (w) w->interval = c->r[1];
}
NVN(nvnWindowSetCrop) {}
NVN(nvnWindowGetCrop) { memset(GPTR(c->r[1]), 0, 16); }
NVN(nvnWindowSetDebugLabel) {}
NVN(nvnWindowAcquireTexture) {  // (window, sync, int* index) -> NVNwindowAcquireTextureResult
    Window* w = get<Window>(c->r[0]);
    uint32_t i = w ? w->next++ % std::max(1u, w->ntex) : 0;
    st32(c->r[2], i);
    c->r[0] = 0;
}
NVN(nvnQueueAcquireTexture) {  // (queue, window, int* index)
    Window* w = get<Window>(c->r[1]);
    uint32_t i = w ? w->next++ % std::max(1u, w->ntex) : 0;
    st32(c->r[2], i);
    c->r[0] = 0;
}
NVN(nvnQueuePresentTexture) {  // (queue, window, index)
    Window* w = get<Window>(c->r[1]);
    Texture* t = w ? get<Texture>(w->textures[c->r[2] % std::max(1u, w->ntex)]) : nullptr;
    if (render_async()) {  // before the pacing: the frame time the game steps by includes this wait
        std::unique_lock<std::mutex> l(g_rq_lock);
        g_rq_done_cv.wait(l, [] { return g_rq_pres_done >= g_rq_pres_enq; });
    }
    pace_present(w ? w->interval : 1);
    Job j{Job::PRESENT};
    j.tex = t;
    job_queue(std::move(j));
}

// -------------------------------------------------------------------------------------------- proc policy
namespace rt {
// pure driver code that only touches the struct it is given: run NVIDIA's own implementation
bool nvn_use_real(const std::string& n) {
    if (nvn_table().count(n)) return false;
    static const char* prefixes[] = {"nvnBlendState", "nvnChannelMaskState", "nvnColorState", "nvnDepthStencilState",
                                     "nvnMultisampleState", "nvnPolygonState", "nvnVertexAttribState",
                                     "nvnVertexStreamState", "nvnTextureView"};
    for (const char* p : prefixes)
        if (!n.compare(0, strlen(p), p)) return true;
    if (n.find("Builder") != std::string::npos) return true;
    static const char* exact[] = {"nvnDeviceGetInteger", "nvnDeviceGetTextureHandle", "nvnDeviceGetImageHandle",
                                  "nvnDeviceGetSeparateTextureHandle", "nvnDeviceGetSeparateSamplerHandle",
                                  "nvnDeviceGetTexelFetchHandle", "nvnTexturePoolInitialize", "nvnTexturePoolFinalize",
                                  "nvnTexturePoolGetMemoryPool", "nvnTexturePoolGetMemoryOffset", "nvnTexturePoolGetSize",
                                  "nvnSamplerPoolGetMemoryPool", "nvnSamplerPoolGetMemoryOffset", "nvnSamplerPoolGetSize",
                                  "nvnSamplerPoolFinalize", "nvnProgramInitialize", "nvnProgramSetSampleShading",
                                  "nvnCommandBufferSetMemoryCallback", "nvnCommandBufferSetMemoryCallbackData",
                                  "nvnCommandBufferGetMemoryCallback", "nvnCommandBufferGetMemoryCallbackData"};
    for (const char* e : exact)
        if (n == e) return true;
    return n.size() > 13 && n.compare(n.size() - 13, 13, "SetDebugLabel") == 0;
}
}  // namespace rt
