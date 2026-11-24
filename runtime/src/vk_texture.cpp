// Textures whose data the game put straight into a memory pool: untile (Tegra block-linear) or copy (pitch-linear)
// from guest memory into a staging ring, then copy into the Vulkan image. Block-linear layout: GOB = 64 bytes x 8
// rows; blocks are 1 GOB wide and 2^n GOBs tall (n chosen per level from its height, at most 16 GOBs), row-major.
// The block-height rule is the common Tegra one and is an assumption for this driver: check textures on screen.
#include "plat.h"
#include "vk.h"

#include <chrono>
#include <filesystem>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;

using namespace rt;

namespace nvn { struct FormatInfo { uint32_t bw, bh, bytes, flags; }; FormatInfo format_info(uint32_t f); uint32_t block_height_max(uint32_t layout); }

namespace vk {

// one upload ring per submit slot (vk.cpp): a slot's ring is reused only after its submit finished on the GPU
static VkBuffer g_stages[3], g_stage;
static uint8_t* g_stage_maps[3], *g_stage_map;
static VkDeviceSize g_stage_cap = 256ull << 20, g_stage_used = 0;
static int g_stage_slot = 0;

void staging_reset(int slot) {
    g_stages[g_stage_slot] = g_stage, g_stage_maps[g_stage_slot] = g_stage_map;
    g_stage_slot = slot;
    g_stage = g_stages[slot], g_stage_map = g_stage_maps[slot];
    g_stage_used = 0;
}

static uint8_t* staging(VkDeviceSize n, VkDeviceSize* off) {
    if (!g_stage) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = g_stage_cap;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &g_stage));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, g_stage, &req);
        VkDeviceSize o;
        VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &o);
        VKC(vkBindBufferMemory(dev, g_stage, m, o));
        void* p;
        VKC(vkMapMemory(dev, m, o, g_stage_cap, 0, &p));
        g_stage_map = (uint8_t*)p;
    }
    VkDeviceSize a = (g_stage_used + 15) & ~15ull;
    if (a + n > g_stage_cap) return nullptr;  // ponytail: ring full -> skip this upload; grow when it happens
    g_stage_used = a + n;
    *off = a;
    return g_stage_map + a;
}

static uint32_t gob_block_height(uint32_t rows, uint32_t bh) {  // GOBs per block for a level with `rows` rows of blocks
    uint32_t gobs = (rows + 7) / 8;
    while (bh > 1 && gobs <= bh / 2) bh /= 2;
    return bh;
}

// copy one level out of block-linear memory into `dst` (tightly packed rows of `row_bytes`)
static void untile(const uint8_t* src, uint8_t* dst, uint32_t row_bytes, uint32_t rows, uint32_t bh) {
    uint32_t blocks_x = (row_bytes + 63) / 64, block_bytes = 512 * bh;
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t by = y / (8 * bh), gob_in_block = (y % (8 * bh)) / 8, yy = y % 8;
        for (uint32_t x = 0; x < row_bytes; x += 16) {
            uint32_t bx = x / 64, xx = x % 64;
            uint32_t off = (by * blocks_x + bx) * block_bytes + gob_in_block * 512 +
                           (xx / 32) * 256 + (yy / 2) * 64 + ((xx % 32) / 16) * 32 + (yy % 2) * 16;
            uint32_t n = std::min(16u, row_bytes - x);
            memcpy(dst + (size_t)y * row_bytes + x, src + off, n);
        }
    }
}

// size of one block-linear level (same rule as nvnTextureBuilderGetStorageSize in nvn_hle.cpp)
static uint32_t level_size(uint32_t row_bytes, uint32_t rows, uint32_t bh) {
    uint32_t blocks_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    return blocks_x * blocks_y * 512 * bh;
}

uint64_t g_submit_serial = 0, g_reuploads = 0;

// nvnTextureWriteTexels: CPU texel writes are kept linear and copied into the image at the texture's next use (any
// target; level 0). The pool storage is not touched, so these textures skip the pool upload.
struct PendingTexels { int32_t rg[6]; std::vector<uint8_t> data; };
static std::mutex g_texels_lock;
static std::unordered_map<uint32_t, std::vector<PendingTexels>> g_texels;  // by guest texture object
static std::atomic<uint32_t> g_texels_count{0};

bool has_texels(nvn::Texture* t) {
    if (!g_texels_count) return false;
    std::lock_guard<std::mutex> l(g_texels_lock);
    return g_texels.count(t->guest) != 0;
}

void apply_texels(VkCommandBuffer cb, nvn::Texture* t, Image* im) {
    std::vector<PendingTexels> v;
    {
        std::lock_guard<std::mutex> l(g_texels_lock);
        auto it = g_texels.find(t->guest);
        if (it == g_texels.end()) return;
        v = std::move(it->second);
        g_texels.erase(it);
        g_texels_count -= (uint32_t)v.size();
    }
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    bool is3d = t->target == 2;
    for (auto& p : v) {
        VkDeviceSize so;
        uint8_t* dst = staging(p.data.size(), &so);
        if (!dst) { log("vk: staging ring full, texels for %08x dropped", t->guest); continue; }
        memcpy(dst, p.data.data(), p.data.size());
        VkBufferImageCopy c{};
        c.bufferOffset = so;
        uint32_t d = (uint32_t)std::max(1, p.rg[5]);
        c.imageSubresource = {(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect, 0,
                              is3d ? 0u : (uint32_t)p.rg[2], is3d ? 1u : d};
        c.imageOffset = {p.rg[0], p.rg[1], is3d ? p.rg[2] : 0};
        c.imageExtent = {(uint32_t)std::max(1, p.rg[3]), (uint32_t)std::max(1, p.rg[4]), is3d ? d : 1u};
        vkCmdCopyBufferToImage(cb, g_stage, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    im->written = true;
    im->from_pool = false;
    static int n = 0;
    if (n++ < 3) log("vk: WriteTexels data copied into %08x (%ux%u, %zu writes)", t->guest, im->w, im->h, v.size());
}

extern std::atomic<uint8_t> g_pack_gen;  // texture packs (below)
// once per submit per texture: GPU writes do not trip the watch, so only CPU-side changes cause a new upload
void check_pool_texture(nvn::Texture* t, Image* im) {
    if (!t || !im->from_pool || im->checked == g_submit_serial) return;
    im->checked = g_submit_serial;
    if (im->pad_style >= 0 && im->pad_style != rt::pad_icon_style()) im->written = false;  // other controller / setting
    if (im->resist >= 0 && im->resist != (int8_t)rt::resist_icons_on()) im->written = false;  // badge icons on / off
    if (im->pack_file && im->pack_gen != g_pack_gen) im->written = false;  // UHD pack switched on / off
    HitchTimer ht{HITCH_CHECK};
    if (mem_written((uint32_t)t->gpu, im->pool_bytes)) { im->written = false; g_reuploads++; }
}

// ------------------------------------------------------------------------------------------------ texture packs
// NX_TEX_DUMP=1: every new 2D texture from guest memory (level 0, layer 0) -> <data>/textures/dump/<w>x<h>_<nvn fmt>_
// <hash>.dds, once per hash. Files <data>/textures/pack/*_<hash>.dds (any size, own mip chain, BCn or RGBA8 DDS)
// replace the texture whose level 0 has that hash: the UHD pack (tools/uhd_pack.py upscales the dumps). NX_TEX_PACK=0
// starts with the pack off; the menus switch it live (rt::set_tex_pack: textures with a pack file upload again).
struct DxgiFmt { VkFormat unorm, srgb; uint32_t dxgi, dxgi_srgb, block; };  // block: bytes per 4x4, 0 = 4 B/pixel
static const DxgiFmt k_dxgi[] = {
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 71, 72, 8},
    {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, 74, 75, 16},
    {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, 77, 78, 16},
    {VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, 80, 80, 8},
    {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, 83, 83, 16},
    {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, 98, 99, 16},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, 28, 29, 0},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, 87, 91, 0},
};
static const DxgiFmt* dxgi_of(VkFormat f, bool* srgb) {
    if (f == VK_FORMAT_BC1_RGB_UNORM_BLOCK) f = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    if (f == VK_FORMAT_BC1_RGB_SRGB_BLOCK) f = VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    for (auto& d : k_dxgi)
        if (f == d.unorm || f == d.srgb) { *srgb = f == d.srgb && d.srgb != d.unorm; return &d; }
    return nullptr;
}
static VkDeviceSize dds_level_size(const DxgiFmt* d, uint32_t w, uint32_t h) {
    return d->block ? (VkDeviceSize)std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * d->block
                    : (VkDeviceSize)w * h * 4;
}
// FNV-1a in 4 interleaved lanes over 32-byte steps (one multiply chain was ~1 GB/s: 300 ms per map load), then the
// tail bytes. Pack and dump file names carry this hash.
static uint64_t tex_hash(const uint8_t* p, size_t n) {
    const uint64_t prime = 0x100000001b3ull;
    uint64_t h[4] = {0xcbf29ce484222325ull, 0x84222325cbf29ce4ull, 0xcbf29ce4ull, 0x84222325ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) h[k] = (h[k] ^ w[k]) * prime;
    }
    for (; i < n; i++) h[0] = (h[0] ^ p[i]) * prime;
    return (h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7)) ^ n;
}
static void dump_dds(const fs::path& file, const DxgiFmt* d, bool srgb, uint32_t w, uint32_t h, const uint8_t* data) {
    uint32_t hdr[32] = {0x20534444, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, h, w, (uint32_t)dds_level_size(d, w, h), 0, 1};
    hdr[19] = 32, hdr[20] = 0x4, hdr[21] = 0x30315844;  // pixel format: FOURCC "DX10"
    hdr[27] = 0x1000;                                   // caps: texture
    uint32_t dx10[5] = {srgb ? d->dxgi_srgb : d->dxgi, 3, 0, 1, 0};
    std::ofstream f(file, std::ios::binary);
    f.write((const char*)hdr, sizeof hdr);
    f.write((const char*)dx10, sizeof dx10);
    f.write((const char*)data, (std::streamsize)dds_level_size(d, w, h));
}

// One per pack file. A worker thread reads the file (a 2048^2 BC7 file with mips is 5.6 MB; reading them on the game
// thread at map loads was the 100-800 ms stutter of 2026-10-27); the texture shows its original data until then. The
// uploaded image is kept per hash and shared by every texture with that hash, so a texture used again (map reloads,
// pool reuse) swaps it in with no read or upload, and no image is made twice.
struct PackTex {
    fs::path file;
    std::atomic<int> state{0};  // 0 not asked, 1 reading, 2 read (data), 3 uploaded (img), 4 unusable
    std::vector<uint8_t> data;
    Image::Own img{};
    int refs = 0;            // images showing it now
    bool in_unused = false;  // in g_pack_unused (no references, image kept for a reuse)
};
static std::unordered_map<uint64_t, PackTex>& pack_index() {
    static std::unordered_map<uint64_t, PackTex> idx = [] {
        std::unordered_map<uint64_t, PackTex> m;
        std::error_code ec;
        // The UHD pack first, then texture mods <data>/mods/<name>/textures (NX_MODS_DISABLED skips them as for romfs):
        // a mod's file wins over the pack for the same hash; between mods the first name wins, as for romfs files.
        // ponytail: mod textures follow the UHD pack switch; a separate switch if people want mods with the pack off.
        std::vector<fs::path> dirs{fs::path(g_data_dir) / "textures" / "pack"};
        std::string off = getenv("NX_MODS_DISABLED") ? std::string(",") + getenv("NX_MODS_DISABLED") + "," : ",";
        std::vector<fs::path> mods;
        for (auto& e : fs::directory_iterator(fs::path(g_data_dir) / "mods", ec))
            if (fs::is_directory(e.path() / "textures", ec) && off.find("," + e.path().filename().string() + ",") == std::string::npos)
                mods.push_back(e.path() / "textures");
        std::sort(mods.rbegin(), mods.rend());  // later dirs overwrite: reverse name order
        dirs.insert(dirs.end(), mods.begin(), mods.end());
        for (const fs::path& dir : dirs) {
            size_t before = m.size(), n_files = 0;
            for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
                std::string n = e.path().filename().string();
                size_t u = n.rfind('_');
                if (u == std::string::npos || e.path().extension() != ".dds") continue;
                // debug: NX_PACK_SKIP=<hex hash>,... leaves those textures original (finding a bad pack texture)
                if (getenv("NX_PACK_SKIP") && strstr(getenv("NX_PACK_SKIP"), n.substr(u + 1, 16).c_str())) continue;
                m[strtoull(n.c_str() + u + 1, nullptr, 16)].file = e.path();
                n_files++;
            }
            if (n_files && dir != dirs[0]) log("textures: mod %s: %zu textures (%zu new)", dir.parent_path().filename().string().c_str(), n_files, m.size() - before);
        }
        if (!m.empty()) log("textures: pack with %zu replacement textures", m.size());
        return m;
    }();
    return idx;
}

static void pack_read_async(PackTex* p) {
    static std::mutex lock;
    static std::condition_variable cv;
    static std::deque<PackTex*> queue;
    static bool started = [] {
        std::thread([] {
            for (;;) {
                PackTex* p;
                {
                    std::unique_lock<std::mutex> l(lock);
                    cv.wait(l, [] { return !queue.empty(); });
                    p = queue.front();
                    queue.pop_front();
                }
                std::ifstream f(p->file, std::ios::binary | std::ios::ate);
                std::vector<uint8_t> b(f ? (size_t)f.tellg() : 0);
                f.seekg(0);
                f.read((char*)b.data(), (std::streamsize)b.size());
                p->data = std::move(b);
                p->state = 2;
            }
        }).detach();
        return true;
    }();
    (void)started;
    p->state = 1;
    {
        std::lock_guard<std::mutex> l(lock);
        queue.push_back(p);
    }
    cv.notify_one();
}

std::atomic<int> g_pack_on{-1};  // -1: not read from NX_TEX_PACK yet
std::atomic<uint8_t> g_pack_gen{0};  // +1 per switch: images uploaded under another value check again
bool pack_on() {
    if (g_pack_on < 0) {
        // tex_pack = auto (or unset, weak-PC default): off on integrated GPUs and under 4 GB of VRAM (4x textures; the
        // spill to system RAM keeps it running but stutters); F1 / the launcher can still turn it on
        const char* e = getenv("NX_TEX_PACK");
        if (e && _stricmp(e, "auto")) g_pack_on = atoi(e) != 0;
        else {
            g_pack_on = !weak_gpu();
            if (!pack_index().empty()) log("textures: UHD pack auto -> %s (%llu MB VRAM)", g_pack_on ? "on" : "off (weak GPU)", (unsigned long long)(vram_bytes() >> 20));
        }
    }
    return g_pack_on != 0;
}

bool pack_arrived(Image* im) {
    auto it = pack_index().find(im->pack_wait);
    return it == pack_index().end() || it->second.state != 1;
}

// Pack images without references (their textures were finalized or got other data) stay for a reuse until they add
// up to NX_PACK_CACHE_MB (default VRAM / 8, 128 .. 1024); then the oldest go and their file is read again when needed. Before
// 2026-10-29 every pack image stayed for the whole run (a full-game pack would fill VRAM).
static std::deque<uint64_t> g_pack_unused;
static VkDeviceSize g_pack_unused_bytes = 0;
static void pack_ref(uint64_t hash, int d) {
    auto it = pack_index().find(hash);
    if (it == pack_index().end()) return;
    PackTex& p = it->second;
    p.refs += d;
    if (d > 0 && p.in_unused) p.in_unused = false, g_pack_unused_bytes -= p.img.mem_size;
    if (p.refs || p.state != 3 || p.in_unused) return;
    p.in_unused = true;
    g_pack_unused.push_back(hash);
    g_pack_unused_bytes += p.img.mem_size;
    // default: an eighth of the VRAM, 128 MB .. 1 GB (a 2 GB card kept 1 GB of unused pack images before)
    static const VkDeviceSize budget = getenv("NX_PACK_CACHE_MB") ? (VkDeviceSize)atoi(getenv("NX_PACK_CACHE_MB")) << 20
                                                                  : std::clamp<VkDeviceSize>(vram_bytes() / 8, 128ull << 20, 1024ull << 20);
    while (g_pack_unused_bytes > budget && !g_pack_unused.empty()) {
        PackTex& q = pack_index()[g_pack_unused.front()];
        g_pack_unused.pop_front();
        if (!q.in_unused) continue;  // in use again
        q.in_unused = false;
        g_pack_unused_bytes -= q.img.mem_size;
        static uint64_t freed = 0;
        if (freed++ % 100 == 0) log("textures: %llu unused pack images freed (cache over %llu MB)", (unsigned long long)freed, (unsigned long long)(budget >> 20));
        defer_destroy(q.img.image, q.img.view, {}, q.img.mem, q.img.mem_off, q.img.mem_size);
        q.img = {};
        q.state = 0;
    }
}
void pack_released(Image* im) { pack_ref(im->pack_hash, -1); }

// put the pack image in place of the texture's own (kept in im->own for when the game reuses the texture)
static void pack_swap_in(Image* im, uint64_t hash, const Image::Own& r) {
    if (!im->own) im->own = new Image::Own{im->image, im->view, im->format, im->w, im->h, im->levels, im->layout, std::move(im->views)};
    else im->own->views = std::move(im->views), im->own->layout = im->layout;  // swapped in again: its views stay its own
    im->image = r.image, im->view = r.view, im->format = r.format, im->w = r.w, im->h = r.h, im->levels = r.levels;
    im->layout = VK_IMAGE_LAYOUT_GENERAL;
    im->views.clear();
    im->view_type = VK_IMAGE_VIEW_TYPE_2D;
    im->replaced = true;
    im->pack_hash = hash;
    pack_ref(hash, 1);
}
static void pack_swap_out(Image* im) {
    Image::Own& o = *im->own;
    defer_destroy(VK_NULL_HANDLE, VK_NULL_HANDLE, std::move(im->views), VK_NULL_HANDLE, 0, 0);  // this image's views of the pack image
    im->image = o.image, im->view = o.view, im->format = o.format, im->w = o.w, im->h = o.h, im->levels = o.levels;
    im->layout = o.layout;
    im->views = std::move(o.views);
    im->view_type = VK_IMAGE_VIEW_TYPE_2D;
    im->replaced = false;
    pack_ref(im->pack_hash, -1);
}

static bool pack_upload(VkCommandBuffer cb, Image* im, PackTex& p, uint64_t hash, bool srgb);
// The pack's image for this hash in place of the texture's: true when done (false: original data this time)
static bool pack_replace(VkCommandBuffer cb, Image* im, uint64_t hash, bool srgb) {
    auto it = pack_index().find(hash);
    if (it == pack_index().end()) return false;
    im->pack_file = true, im->pack_gen = g_pack_gen;
    if (!pack_on()) return false;
    PackTex& p = it->second;
    if (p.state == 0) pack_read_async(&p);
    if (p.state == 1) { im->pack_wait = hash; return false; }
    if (p.state == 4) return false;
    if (p.state == 3) { pack_swap_in(im, hash, p.img); return true; }
    return pack_upload(cb, im, p, hash, srgb);
}
// p.data (a DDS file's bytes, state 2) -> its image, swapped in (state 3; 4 = unusable)
static bool pack_upload(VkCommandBuffer cb, Image* im, PackTex& p, uint64_t hash, bool srgb) {
    std::vector<uint8_t> b = std::move(p.data);
    p.state = 4;
    uint32_t hdr[32];
    if (b.size() < 128) return false;
    memcpy(hdr, b.data(), 128);
    if (hdr[0] != 0x20534444) return false;
    uint32_t h = hdr[3], w = hdr[4], levels = std::max(1u, hdr[7]), dxgi = 0;
    size_t pos = 128;
    if (hdr[21] == 0x30315844 && b.size() >= 148) memcpy(&dxgi, b.data() + 128, 4), pos = 148;
    else if (hdr[21] == 0x31545844) dxgi = 71;  // "DXT1"
    else if (hdr[21] == 0x35545844) dxgi = 77;  // "DXT5"
    const DxgiFmt* d = nullptr;
    for (auto& x : k_dxgi)
        if (dxgi == x.dxgi || dxgi == x.dxgi_srgb) d = &x;
    if (!d) {
        log("textures: %s: DXGI format %u not supported", p.file.string().c_str(), dxgi);
        return false;
    }
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = std::max(1u, w >> l), lh = std::max(1u, h >> l);
        VkDeviceSize n = dds_level_size(d, lw, lh), so;
        if (pos + n > b.size()) { levels = l; break; }
        uint8_t* dst = staging(n, &so);
        if (!dst) { p.data = std::move(b), p.state = 2; return false; }  // staging ring full: next time
        memcpy(dst, b.data() + pos, n);
        pos += n;
        VkBufferImageCopy c{};
        c.bufferOffset = so;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        c.imageExtent = {lw, lh, 1};
        regions.push_back(c);
    }
    if (!levels) return false;
    p.img = new_image_2d(srgb ? d->srgb : d->unorm, w, h, levels);
    p.state = 3;  // before the swap: pack_ref counts only uploaded images
    pack_swap_in(im, hash, p.img);
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdCopyBufferToImage(cb, g_stage, im->image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    p.state = 3;
    static int logged = 0;
    if (logged++ < 20 || getenv("NX_PACK_LOG"))  // NX_PACK_LOG=1: every one (debug)
        log("textures: %016llx -> %s (%ux%u, %u levels)", (unsigned long long)hash, p.file.filename().string().c_str(), w, h, levels);
    return true;
}

// BC3 (DXT5) -> RGBA8, for the field prompts
static std::vector<uint8_t> decode_bc3(const uint8_t* src, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out((size_t)w * h * 4);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++, src += 16) {
            uint8_t a[8] = {src[0], src[1]};
            for (int j = 0; j < 6; j++)
                a[2 + j] = a[0] > a[1] ? (uint8_t)(((6 - j) * a[0] + (1 + j) * a[1]) / 7) : (uint8_t)(((4 - j) * a[0] + (1 + j) * a[1]) / 5);
            if (a[0] <= a[1]) a[6] = 0, a[7] = 255;
            uint64_t ai = 0;
            for (int k = 0; k < 6; k++) ai |= (uint64_t)src[2 + k] << (8 * k);
            const uint16_t c0 = (uint16_t)(src[8] | src[9] << 8), c1 = (uint16_t)(src[10] | src[11] << 8);
            uint8_t c[4][3];
            for (int k = 0; k < 2; k++) {
                const uint16_t v = k ? c1 : c0;
                c[k][0] = (uint8_t)((v >> 11) * 255 / 31), c[k][1] = (uint8_t)(((v >> 5) & 63) * 255 / 63), c[k][2] = (uint8_t)((v & 31) * 255 / 31);
            }
            for (int k = 0; k < 3; k++) c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3), c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
            const uint32_t ci = (uint32_t)(src[12] | src[13] << 8 | src[14] << 16 | (uint32_t)src[15] << 24);
            for (int t = 0; t < 16; t++) {
                const uint32_t x = bx * 4 + t % 4, y = by * 4 + t / 4;
                if (x >= w || y >= h) continue;
                uint8_t* d = &out[((size_t)y * w + x) * 4];
                memcpy(d, c[(ci >> (2 * t)) & 3], 3);
                d[3] = a[(ai >> (3 * t)) & 7];
            }
        }
    return out;
}

// A field prompt with the controller's (A) drawn in: RGBA8 image with a box-filtered mip chain, made once per prompt
// and style and kept (50 prompts x 340 KB at most); swapped in like a pack image.
static std::unordered_map<uint64_t, PackTex> g_prompts;
static bool prompt_replace(VkCommandBuffer cb, Image* im, uint64_t hash, int style, uint32_t w, uint32_t h, const uint8_t* bc3, bool srgb) {
    const uint64_t key = hash + (uint64_t)style * 0x9E3779B97F4A7C15ull;  // never a pack hash (pack_ref ignores it)
    PackTex& p = g_prompts[key];
    if (p.state == 3) { pack_swap_in(im, key, p.img); return true; }
    if (p.state == 4) return false;
    std::vector<uint8_t> px = decode_bc3(bc3, w, h);
    if (!rt::pad_icon_prompt(style, w, h, px.data())) { p.state = 4; return false; }
    uint32_t hdr[32] = {0x20534444, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000, h, w, w * 4, 0, 0};
    hdr[19] = 32, hdr[20] = 0x4, hdr[21] = 0x30315844, hdr[27] = 0x1000;
    uint32_t dx10[5] = {28, 3, 0, 1, 0};  // RGBA8 (pack_upload picks sRGB from the texture)
    std::vector<uint8_t> dds((uint8_t*)hdr, (uint8_t*)hdr + sizeof hdr);
    dds.insert(dds.end(), (uint8_t*)dx10, (uint8_t*)dx10 + sizeof dx10);
    uint32_t lw = w, lh = h, levels = 0;
    for (;;) {
        dds.insert(dds.end(), px.begin(), px.end());
        levels++;
        if (lw == 1 && lh == 1) break;
        const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        std::vector<uint8_t> m((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++) {
                const uint32_t x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1), y0 = std::min(2 * y, lh - 1), y1 = std::min(2 * y + 1, lh - 1);
                for (int k = 0; k < 4; k++)
                    m[((size_t)y * nw + x) * 4 + k] = (uint8_t)((px[((size_t)y0 * lw + x0) * 4 + k] + px[((size_t)y0 * lw + x1) * 4 + k] +
                                                                 px[((size_t)y1 * lw + x0) * 4 + k] + px[((size_t)y1 * lw + x1) * 4 + k] + 2) / 4);
            }
        px = std::move(m), lw = nw, lh = nh;
    }
    memcpy(&dds[28], &levels, 4);
    p.data = std::move(dds), p.state = 2;
    static int logged = 0;
    if (logged++ < 8) log("textures: field prompt %016llx drawn with %s icons", (unsigned long long)hash, style == 1 ? "PlayStation" : "Xbox");
    return pack_upload(cb, im, p, key, srgb);
}

// A movie frame plane (linear 1280x720 Y / 640x360 UV) of a 4K replacement movie (hle_movie.cpp): the HD plane goes
// into this texture's own HD image (one per guest address, kept), swapped in like a pack image.
static bool movie_replace(VkCommandBuffer cb, nvn::Texture* t, Image* im, uint32_t row) {
    const uint32_t stride = t->stride ? t->stride : row;
    std::shared_ptr<std::vector<uint8_t>> nv12;
    size_t off;
    uint32_t w, h;
    if (!rt::movie_hd_plane(rt::movie_fp((const uint8_t*)GPTR((uint32_t)t->gpu), row, t->height, stride), &nv12, &off, &w, &h)) return false;
    const VkDeviceSize n = (VkDeviceSize)w * h * (row / t->width);
    if (off + n > nv12->size()) return false;
    struct MovieImg { Image::Own img; bool fresh = true; };
    static std::unordered_map<uint64_t, MovieImg> imgs;  // ponytail: never freed; 2-3 textures per movie player (~37 MB)
    const uint64_t key = 0x4d4f5649450000ull ^ t->gpu;   // never a pack hash (pack_ref ignores it)
    MovieImg& m = imgs[key];
    if (m.img.image && (m.img.w != w || m.img.h != h || m.img.format != im->format)) {
        // the address held another movie's plane of another size (title Y -> camp UV): that texture stayed 720p for good,
        // so every other camp frame had 720p colour (flowers flickered, user 11-22)
        log("textures: movie plane %08x: 4K image %ux%u -> %ux%u (address reused)", t->guest, m.img.w, m.img.h, w, h);
        defer_destroy(m.img.image, m.img.view, {}, m.img.mem, m.img.mem_off, m.img.mem_size);
        m = MovieImg{};
    }
    if (!m.img.image) m.img = new_image_2d(im->format, w, h, 1);
    VkDeviceSize so;
    uint8_t* dst = staging(n, &so);
    if (!dst) return false;
    memcpy(dst, nv12->data() + off, n);
    pack_swap_in(im, key, m.img);
    if (m.fresh) im->layout = VK_IMAGE_LAYOUT_UNDEFINED, m.fresh = false;
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    VkMemoryBarrier rb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &rb, 0, nullptr, 0, nullptr);
    VkBufferImageCopy c{};
    c.bufferOffset = so;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, g_stage, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    static int logged = 0;
    if (logged++ < 4) log("textures: movie plane %ux%u -> 4K %ux%u (%08x)", t->width, t->height, w, h, t->guest);
    return true;
}

bool upload_from_pool(VkCommandBuffer cb, nvn::Texture* t, Image* im) {
    if (!t->gpu || (im->written && !im->pack_wait)) return false;
    HitchTimer ht{HITCH_UPLOAD};
    im->pack_wait = 0;
    if (im->replaced) pack_swap_out(im);  // the texture's own image and level count for the new data
    if (im->scale != 1) { im->written = true; return false; }  // a render target sampled before any pass wrote it
    nvn::FormatInfo fi = nvn::format_info(t->format);
    bool linear = (t->flags & 0x50) != 0;
    std::vector<VkBufferImageCopy> regions;
    uint32_t levels = linear ? 1 : im->levels;  // linear textures have one level
    std::vector<uint8_t*> dsts(levels);
    for (uint32_t l = 0; l < levels; l++) {  // staging is level-major (one copy region per level, all layers)
        uint32_t w = std::max(1u, t->width >> l), h = std::max(1u, t->height >> l);
        uint32_t bx = (w + fi.bw - 1) / fi.bw, by = (h + fi.bh - 1) / fi.bh;
        VkDeviceSize so;
        dsts[l] = staging((VkDeviceSize)bx * fi.bytes * by * im->layers, &so);
        if (!dsts[l]) { log("vk: staging ring full, texture %08x not uploaded", t->guest); return false; }
        VkBufferImageCopy c{};
        c.bufferOffset = so;
        c.imageSubresource = {(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect, l, 0, im->layers};
        c.imageExtent = {w, h, 1};
        regions.push_back(c);
    }
    // guest memory is layer-major: every level of layer 0, then layer 1, ...; a block-linear layer starts on a
    // level-0 block boundary (Tegra rule, as in Ryujinx's SizeCalculator.AlignLayerSize)
    // level 0 of a texture the pack may replace goes through cached memory: hashing it in the staging ring
    // (write-combined, ~80 MB/s to read) cost 100-400 ms per map load
    static const bool dumping = getenv("NX_TEX_DUMP") && atoi(getenv("NX_TEX_DUMP"));
    static thread_local std::vector<uint8_t> level0;
    const bool keep0 = !linear && t->target == 1 && im->layers == 1 &&
                       (dumping || !pack_index().empty() || rt::pad_icon_atlas_size(t->width, t->height) ||
                        (t->width == 512 && t->height == 128));  // field prompt size
    uint64_t src_off = 0;
    uint32_t bh_max = nvn::block_height_max(t->layout);
    uint32_t block0 = 512 * gob_block_height((std::max(1u, t->height) + fi.bh - 1) / fi.bh, bh_max);
    for (uint32_t layer = 0; layer < im->layers; layer++) {
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t w = std::max(1u, t->width >> l), h = std::max(1u, t->height >> l);
            uint32_t bx = (w + fi.bw - 1) / fi.bw, by = (h + fi.bh - 1) / fi.bh;
            uint32_t row = bx * fi.bytes;
            uint8_t* dst = dsts[l] + (size_t)layer * row * by;
            if (keep0 && l == 0 && layer == 0) level0.resize((size_t)row * by), dst = level0.data();
            const uint8_t* src = (const uint8_t*)GPTR((uint32_t)(t->gpu + src_off));
            if (linear) {
                uint32_t stride = t->stride ? t->stride : row;
                for (uint32_t y = 0; y < by; y++) memcpy(dst + (size_t)y * row, src + (size_t)y * stride, row);
                src_off += (uint64_t)stride * by;
            } else {
                uint32_t bh = gob_block_height(by, bh_max);
                untile(src, dst, row, by, bh);
                src_off += level_size(row, by, bh);
            }
        }
        if (!linear && im->layers > 1) src_off = (src_off + block0 - 1) / block0 * block0;
    }
    if (keep0) memcpy(dsts[0], level0.data(), level0.size());
    im->pool_bytes = (uint32_t)src_off;
    mem_written((uint32_t)t->gpu, im->pool_bytes);  // start watching from this upload
    bool srgb = false;
    const DxgiFmt* dx = !linear && t->target == 1 && im->layers == 1 ? dxgi_of(im->format, &srgb) : nullptr;
    if (dx) {
        if (!keep0) goto upload;
        uint64_t hash = tex_hash(level0.data(), (size_t)dds_level_size(dx, t->width, t->height));
        if (dumping) {
            static std::unordered_set<uint64_t> seen;
            if (seen.insert(hash).second) {
                fs::path dir = fs::path(g_data_dir) / "textures" / "dump";
                char name[64];
                snprintf(name, sizeof name, "%ux%u_%02x_%016llx.dds", t->width, t->height, t->format, (unsigned long long)hash);
                std::error_code ec;
                fs::create_directories(dir, ec);
                if (!fs::exists(dir / name, ec)) dump_dds(dir / name, dx, srgb, t->width, t->height, level0.data());
            }
        }
        if (!dx->block && rt::is_resist_atlas(hash)) {  // resistance badges as icons (pad_icons.cpp)
            im->resist = (int8_t)rt::resist_icons_on();
            im->resist_hash = hash;
            // drawn into level0 itself: the Stats screen atlas also gets button icons (below), over these
            if (im->resist && rt::resist_icon_atlas(hash, t->width, t->height, level0.data())) {
                memcpy(dsts[0], level0.data(), level0.size());
                log("textures: resistance atlas %016llx (%08x) drawn with icons", (unsigned long long)hash, t->guest);
                if (dumping) {
                    char name[64];
                    snprintf(name, sizeof name, "resist_%016llx.dds", (unsigned long long)hash);
                    dump_dds(fs::path(g_data_dir) / "textures" / "dump" / name, dx, srgb, t->width, t->height, level0.data());
                }
            }
            if (!rt::is_pad_icon_atlas(hash)) goto upload;  // a UHD pack file of the atlas would show the letters
        }
        if (!dx->block && rt::is_pad_icon_atlas(hash)) {  // controller icons (pad_icons.cpp)
            im->pad_style = (int8_t)rt::pad_icon_style();
            if (im->pad_style) {
                std::vector<uint8_t>& a = level0;  // painted in place (scratch; dsts[0] already has the original)
                const auto t0 = std::chrono::steady_clock::now();
                if (rt::pad_icon_atlas(im->pad_style, hash, t->width, t->height, a.data())) {
                    memcpy(dsts[0], a.data(), a.size());
                    static int logged = 0;
                    if (logged++ < 16) log("textures: button atlas %016llx (%08x) drawn with %s icons (%.1f ms)", (unsigned long long)hash, t->guest, im->pad_style == 1 ? "PlayStation" : "Xbox",
                                           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                    if (dumping) {
                        char name[64];
                        snprintf(name, sizeof name, "padicons_%d_%016llx.dds", im->pad_style, (unsigned long long)hash);
                        dump_dds(fs::path(g_data_dir) / "textures" / "dump" / name, dx, srgb, t->width, t->height, a.data());
                    }
                }
            }
            goto upload;  // a UHD pack file of the atlas would show Switch icons
        }
        if (dx->unorm == VK_FORMAT_BC3_UNORM_BLOCK && rt::is_pad_prompt(hash)) {  // field prompts "(A) Talk" (pad_icons.cpp)
            im->pad_style = (int8_t)rt::pad_icon_style();
            if (im->pad_style && prompt_replace(cb, im, hash, im->pad_style, t->width, t->height, level0.data(), srgb)) {
                im->written = im->from_pool = true;
                return true;
            }
            goto upload;  // a UHD pack file of the prompt would show the Switch (A)
        }
        if (pack_replace(cb, im, hash, srgb)) { im->written = im->from_pool = true; return true; }
    }
    if (linear && rt::movie_hd_any() && im->layers == 1 && ((t->width == 1280 && t->height == 720) || (t->width == 640 && t->height == 360)) &&
        movie_replace(cb, t, im, ((t->width + fi.bw - 1) / fi.bw) * fi.bytes)) {
        im->written = im->from_pool = true;
        return true;
    }
upload:
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdCopyBufferToImage(cb, g_stage, im->image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    im->written = true;
    im->from_pool = true;
    return true;
}

}  // namespace vk

void nvn::write_texels(nvn::Texture* t, const int32_t rg[6], const uint8_t* data) {
    nvn::FormatInfo fi = nvn::format_info(t->format);
    size_t n = (size_t)((std::max(1, rg[3]) + fi.bw - 1) / fi.bw) * fi.bytes * ((std::max(1, rg[4]) + fi.bh - 1) / fi.bh) *
               std::max(1, rg[5]);
    vk::PendingTexels p;
    memcpy(p.rg, rg, sizeof p.rg);
    p.data.assign(data, data + n);
    std::lock_guard<std::mutex> l(vk::g_texels_lock);
    vk::g_texels[t->guest].push_back(std::move(p));
    vk::g_texels_count++;
}

// settings menus: UHD pack on / off without a restart
namespace rt {
bool tex_pack_on() { return vk::pack_on(); }
void set_tex_pack(bool on) {
    save_setting("tex_pack", on ? "1" : "0");
    vk::g_pack_on = on;
    vk::g_pack_gen++;
}
}  // namespace rt
