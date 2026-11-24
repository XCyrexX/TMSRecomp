// Maxwell shaders -> SPIR-V through the shaderc sidecar (tools/shaderc, Ryujinx's MIT translator), then Vulkan
// shader modules + descriptor layouts per NVN program.
#include "plat.h"
#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
extern char** environ;
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "vk.h"
#include "vk_shader.h"

using namespace rt;

namespace vk {

#ifdef _WIN32
static HANDLE g_in_w, g_out_r;
#else
static int g_in_w = -1, g_out_r = -1;
static bool file_exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
#endif
static std::mutex g_sc_lock;

// The translator: <exe dir>/shaderc (release ZIP) or <data>/shaderc (built from source). Copies keep the file time,
// so the cache stamp below stays the same (compared in whole seconds: ZIP keeps no fractions).
static std::string exe_dir() {
#ifdef _WIN32
    char exe[MAX_PATH];
    std::string d(exe, GetModuleFileNameA(nullptr, exe, MAX_PATH));
    return d.substr(0, d.find_last_of("\\/"));
#else
    return plat::exe_path().parent_path().string();
#endif
}
std::string seed_dir() { return exe_dir() + "/cache_seed"; }
static std::string shaderc_dll() {
    std::string d = exe_dir() + "/shaderc/NxShader.dll";
#ifdef _WIN32
    return GetFileAttributesA(d.c_str()) != INVALID_FILE_ATTRIBUTES ? d : g_data_dir + "/shaderc/NxShader.dll";
#else
    return file_exists(d) ? d : g_data_dir + "/shaderc/NxShader.dll";
#endif
}
#ifndef _WIN32
// Linux: a self-contained publish (-r linux-x64: NxShader next to libhostfxr.so, no .NET install) or the
// framework-dependent dll through dotnet. Pipes for stdin / stdout, stderr shared; posix_spawn, so the big guest
// reservation is never copied.
static void spawn() {
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) fatal("shader translator: pipe failed");
    const std::string dll = shaderc_dll(), sc = dll.substr(0, dll.size() - 4),
                      host = dll.substr(0, dll.find_last_of('/')) + "/libhostfxr.so";
    std::vector<std::string> args = file_exists(host) && access(sc.c_str(), X_OK) == 0 ? std::vector<std::string>{sc}
                                                                                         : std::vector<std::string>{"dotnet", dll};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    pid_t pid;
    const int e = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (e) fatal("cannot start the shader translator (%s): %s", args.back().c_str(), strerror(e));
    close(in[0]);
    close(out[1]);
    g_in_w = in[1];
    g_out_r = out[0];
    log("vk: shader translator started (%s)", args[0].c_str());
}

static void wr(const void* p, size_t n) {
    for (size_t done = 0; done < n;) {
        ssize_t w = write(g_in_w, (const char*)p + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) fatal("shader translator: write failed");
        done += (size_t)w;
    }
}
static void rd(void* p, size_t n) {
    for (size_t got = 0; got < n;) {
        ssize_t r = read(g_out_r, (char*)p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) fatal("shader translator: read failed (did it crash?)");
        got += (size_t)r;
    }
}
#else
static void spawn() {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r, out_w;
    CreatePipe(&in_r, &g_in_w, &sa, 1 << 20);
    CreatePipe(&g_out_r, &out_w, &sa, 1 << 20);
    SetHandleInformation(g_in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    // release ZIP: self-contained NxShader.exe (no .NET install); dev build: framework-dependent dll via dotnet
    const std::string dll = shaderc_dll(), sc = dll.substr(0, dll.size() - 4) + ".exe",
                      host = dll.substr(0, dll.find_last_of("\/")) + "/hostfxr.dll";
    std::string cmd = GetFileAttributesA(host.c_str()) != INVALID_FILE_ATTRIBUTES ? "\"" + sc + "\"" : "dotnet \"" + dll + "\"";
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        fatal("cannot start the shader translator (%s): error %lu", cmd.c_str(), GetLastError());
    CloseHandle(in_r);
    CloseHandle(out_w);
    log("vk: shader translator started");
}

static void wr(const void* p, size_t n) {
    DWORD w;
    if (!WriteFile(g_in_w, p, (DWORD)n, &w, nullptr) || w != n) fatal("shader translator: write failed");
}
static void rd(void* p, size_t n) {
    size_t got = 0;
    while (got < n) {
        DWORD r;
        if (!ReadFile(g_out_r, (char*)p + got, (DWORD)(n - got), &r, nullptr) || !r) fatal("shader translator: read failed (did it crash?)");
        got += r;
    }
}
#endif
template <class T> T rdv() { T v; rd(&v, sizeof v); return v; }

// ------------------------------------------------------------------------------------------------- translation cache
// <data>/cache/shaders.bin: every translation, so later runs skip the sidecar (100+ ms hitches per new shader).
// A shader's length is only known after translating it, so entries are found by a hash of the first 256 code bytes +
// the inputs, then checked against a hash of the full code (code_size bytes) and its constant data. The header
// stamps the translator build (NxShader.dll size + time): a rebuilt translator starts a new file.
static uint64_t fnv(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 0x100000001b3ull;
    return h;
}
struct CacheEntry { uint64_t full; uint32_t code_size; Translated t; };
static std::unordered_multimap<uint64_t, CacheEntry> g_tcache;
static FILE* g_tcache_file = nullptr;
std::atomic<uint32_t> g_translate_us{0}, g_translate_n{0}, g_translate_hits{0};  // since the last frame stats line

static void tcache_open() {
    static bool done = false;
    if (done) return;
    done = true;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExA(shaderc_dll().c_str(), GetFileExInfoStandard, &a);
    uint64_t stamp[3] = {0x31435358ull /* "XSC1" */, ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow,
                         ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime};
#else
    // the same stamp as on Windows (FILETIME: 100 ns since 1601), as shaderscan.py writes it
    struct stat st{};
    stat(shaderc_dll().c_str(), &st);
    uint64_t stamp[3] = {0x31435358ull /* "XSC1" */, (uint64_t)st.st_size,
                         ((uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec) / 100 + 116444736000000000ull};
#endif
    std::string dir = g_data_dir + "/cache", path = dir + "/shaders.bin";
    CreateDirectoryA(dir.c_str(), nullptr);
    // a release may ship <exe dir>/cache_seed/shaders.bin (the shaders of a full play): read first, never written
    for (const std::string& p : {seed_dir() + "/shaders.bin", path})
    if (FILE* f = fopen(p.c_str(), "rb")) {
        uint64_t h[3];
        bool ok = fread(h, sizeof h, 1, f) == 1 && h[0] == stamp[0] && h[1] == stamp[1] && h[2] / 10000000 == stamp[2] / 10000000;
        for (uint64_t prefix; ok && fread(&prefix, 8, 1, f) == 1;) {
            CacheEntry e;
            uint32_t n, nres;
            ok = fread(&e.full, 8, 1, f) && fread(&e.code_size, 4, 1, f) && fread(&n, 4, 1, f);
            if (!ok) break;
            e.t.code_size = e.code_size;
            e.t.spirv.resize(n);
            ok = fread(e.t.spirv.data(), 4, n, f) == n && fread(&nres, 4, 1, f);
            if (!ok) break;
            e.t.res.resize(nres);
            ok = fread(e.t.res.data(), sizeof(Resource), nres, f) == nres && fread(&e.t.frag_outputs, 4, 1, f);
            if (ok) g_tcache.emplace(prefix, std::move(e));
        }
        fclose(f);
        log("vk: shader cache %s: %zu translations loaded%s (translator %s)", p.c_str(), g_tcache.size(),
            ok || g_tcache.size() ? "" : " (none: other translator build)", shaderc_dll().c_str());
        if (!ok && g_tcache.empty() && p == path) DeleteFileA(path.c_str());
    }
#ifdef _WIN32
    bool fresh = GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES;
#else
    bool fresh = !file_exists(path);
#endif
    g_tcache_file = fopen(path.c_str(), "ab");
    if (g_tcache_file && fresh) fwrite(stamp, sizeof stamp, 1, g_tcache_file), fflush(g_tcache_file);
}

static bool translate_sidecar(uint32_t stage, const uint8_t* code, uint32_t len, const uint32_t local[3], uint32_t consts_off, uint32_t consts_size, Translated& out, const uint8_t* attr_types);

// SPIR-V content hash of every module made here: pipeline recipes (vk_replay.cpp) name their shaders by it
static std::mutex g_mh_lock;
static std::unordered_map<VkShaderModule, uint64_t> g_module_hash;
uint64_t spirv_hash(const Translated& t) { return fnv(t.spirv.data(), t.spirv.size() * 4); }
// Integer texture reads. Maxwell TEX puts raw texel bits in registers; the translator declares every sampler float and
// keeps the bits with floatBitsToUint, then I2F. A float sampler over a stencil view (the shadow maps' character ids:
// a character's own id counts as lit) is undefined in Vulkan and never gave the id on NVIDIA, so every character
// stayed self-shadowed: darker skin and blotches in battle (user 11-06). Samplers whose result reaches a bitcast to int
// + int-to-float conversion are turned into uint samplers here; each sample's uvec4 is bitcast back to the vec4 the rest
// of the shader expects, so the bits are unchanged. Applied in make_module: spirv_hash stays the sidecar's, recipes hold.
static std::vector<uint32_t> integer_samplers(const std::vector<uint32_t>& in) {
    std::unordered_map<uint32_t, uint32_t> var_of_load, from_sampler, ptr_base, stored, cast, ptr_type;
    std::unordered_map<uint32_t, uint32_t> sampled_of_ptr;  // UniformConstant pointer -> OpTypeSampledImage
    std::unordered_map<uint32_t, uint32_t> image_of_sampled;
    std::unordered_map<uint32_t, size_t> image_at;
    std::vector<uint32_t> marked;
    uint32_t tu = 0, tf = 0, tuvec4 = 0;
    for (size_t w = 5; w < in.size();) {
        const uint32_t op = in[w] & 0xFFFF, n = in[w] >> 16;
        if (!n) return {};
        switch (op) {
            case 21: if (in[w + 2] == 32 && !in[w + 3]) tu = in[w + 1]; break;                          // OpTypeInt
            case 22: if (in[w + 2] == 32) tf = in[w + 1]; break;                                        // OpTypeFloat
            case 23: if (in[w + 2] == tu && in[w + 3] == 4) tuvec4 = in[w + 1]; break;                  // OpTypeVector
            case 25: image_at[in[w + 1]] = w; break;                                                    // OpTypeImage
            case 27: image_of_sampled[in[w + 1]] = in[w + 2]; break;                                    // OpTypeSampledImage
            case 32: if (!in[w + 2] && image_of_sampled.count(in[w + 3])) sampled_of_ptr[in[w + 1]] = in[w + 3]; break;  // OpTypePointer UniformConstant
            case 59: if (sampled_of_ptr.count(in[w + 1])) ptr_type[in[w + 2]] = in[w + 1]; break;       // OpVariable
            case 61:                                                                                    // OpLoad
                if (ptr_type.count(in[w + 3])) var_of_load[in[w + 2]] = in[w + 3];
                else {
                    auto base = ptr_base.find(in[w + 3]);
                    auto st = stored.find(base == ptr_base.end() ? in[w + 3] : base->second);
                    if (st != stored.end()) from_sampler[in[w + 2]] = st->second;
                }
                break;
            case 62: {                                                                                  // OpStore
                auto f = from_sampler.find(in[w + 2]);
                if (f != from_sampler.end()) {
                    auto base = ptr_base.find(in[w + 1]);
                    stored[base == ptr_base.end() ? in[w + 1] : base->second] = f->second;
                }
                break;
            }
            case 65: ptr_base[in[w + 2]] = in[w + 3]; break;                                            // OpAccessChain
            case 79: case 81: case 83: {                                                                // OpVectorShuffle, OpCompositeExtract, OpCopyObject
                auto f = from_sampler.find(in[w + 3]);
                if (f != from_sampler.end()) from_sampler[in[w + 2]] = f->second;
                break;
            }
            case 87: case 88: case 96: {                                                                // sample implicit, explicit lod, gather
                auto v = var_of_load.find(in[w + 3]);
                if (v != var_of_load.end()) from_sampler[in[w + 2]] = v->second;
                break;
            }
            case 124: {                                                                                 // OpBitcast
                auto f = from_sampler.find(in[w + 3]);
                if (f != from_sampler.end()) cast[in[w + 2]] = f->second;
                break;
            }
            case 111: case 112: {                                                                       // OpConvertSToF, OpConvertUToF
                auto f = cast.find(in[w + 3]);
                if (f != cast.end() && !std::count(marked.begin(), marked.end(), f->second)) marked.push_back(f->second);
                break;
            }
        }
        w += n;
    }
    if (marked.empty() || !tu || !tf) return {};
    static int told = 0;
    if (told++ < 5) log("vk: %zu integer sampler(s) in a shader (%zu words)", marked.size(), in.size());
    uint32_t bound = in[3];
    const uint32_t uvec4 = tuvec4 ? tuvec4 : bound++;
    std::unordered_map<uint32_t, uint32_t> new_sampled, new_ptr;  // original sampled image type -> new ones
    for (uint32_t v : marked) {
        const uint32_t st = sampled_of_ptr[ptr_type[v]];
        if (!new_sampled.count(st)) new_sampled[st] = bound++, new_ptr[st] = bound++;
    }
    std::vector<uint32_t> o(in.begin(), in.begin() + 5);
    o.reserve(in.size() + 64);
    bool uvec_done = tuvec4 != 0;
    for (size_t w = 5; w < in.size();) {
        const uint32_t op = in[w] & 0xFFFF, n = in[w] >> 16;
        const size_t at = o.size();
        o.insert(o.end(), in.begin() + w, in.begin() + w + n);
        if (op == 27 && new_sampled.count(in[w + 1])) {  // OpTypeSampledImage: the uint twins right behind it
            const uint32_t st = in[w + 1];
            if (!uvec_done) uvec_done = true, o.insert(o.end(), {4u << 16 | 23, uvec4, tu, 4});
            const size_t im = image_at[in[w + 2]];
            const uint32_t nimg = bound++;
            o.push_back(in[im]), o.push_back(nimg), o.push_back(tu);
            o.insert(o.end(), in.begin() + im + 3, in.begin() + im + (in[im] >> 16));
            o.insert(o.end(), {3u << 16 | 27, new_sampled[st], nimg, 4u << 16 | 32, new_ptr[st], 0, new_sampled[st]});
        } else if (op == 59 && std::count(marked.begin(), marked.end(), in[w + 2])) {  // OpVariable
            o[at + 1] = new_ptr[sampled_of_ptr[in[w + 1]]];
        } else if (op == 61 && std::count(marked.begin(), marked.end(), in[w + 3])) {  // OpLoad
            o[at + 1] = new_sampled[sampled_of_ptr[ptr_type[in[w + 3]]]];
        } else if (op == 87 || op == 88 || op == 96) {
            auto v = var_of_load.find(in[w + 3]);
            if (v != var_of_load.end() && std::count(marked.begin(), marked.end(), v->second)) {
                const uint32_t raw = bound++;
                o[at + 1] = uvec4, o[at + 2] = raw;
                o.insert(o.end(), {4u << 16 | 124, in[w + 1], in[w + 2], raw});  // OpBitcast back to the original vec4
            }
        }
        w += n;
    }
    o[3] = bound;
    return o;
}
static std::unordered_map<uint64_t, VkShaderModule> g_module_by_hash;  // one module per SPIR-V hash; g_mh_lock
VkShaderModule make_module(const Translated& t, bool shared) {
    const uint64_t h = spirv_hash(t);
    if (shared) { std::lock_guard<std::mutex> l(g_mh_lock); if (auto it = g_module_by_hash.find(h); it != g_module_by_hash.end()) return it->second; }
    const std::vector<uint32_t> patched = integer_samplers(t.spirv);
    const std::vector<uint32_t>& code = patched.empty() ? t.spirv : patched;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m;
    VKC(vkCreateShaderModule(dev, &ci, nullptr, &m));
    std::lock_guard<std::mutex> l(g_mh_lock);
    g_module_hash[m] = h;
    if (shared) {
        auto [it, added] = g_module_by_hash.emplace(h, m);
        if (!added) { g_module_hash.erase(m); vkDestroyShaderModule(dev, m, nullptr); return it->second; }  // made by another thread meanwhile
    }
    return m;
}
uint64_t module_hash(VkShaderModule m) {
    std::lock_guard<std::mutex> l(g_mh_lock);
    auto it = g_module_hash.find(m);
    return it == g_module_hash.end() ? 0 : it->second;
}
const Translated* translation_by_hash(uint64_t h) {  // from shaders.bin as loaded (and translated since)
    std::lock_guard<std::mutex> l(g_sc_lock);
    tcache_open();
    static std::unordered_map<uint64_t, const Translated*> idx;
    static size_t indexed = 0;
    if (indexed != g_tcache.size()) {  // node pointers stay valid when the map grows
        for (auto& [k, e] : g_tcache) idx.emplace(spirv_hash(e.t), &e.t);
        indexed = g_tcache.size();
    }
    auto it = idx.find(h);
    return it == idx.end() ? nullptr : it->second;
}

std::atomic<uint32_t> g_pipeline_us{0}, g_pipeline_n{0}, g_pipeline_inline{0},g_programs_new{0}, g_programs_shared{0}, g_programs_moved{0};
static VkPipelineCache g_pcache = VK_NULL_HANDLE;
static uint32_t g_pcache_saved_n = 0;  // pipelines created when last saved

static std::vector<char> pcache_file() {
    std::vector<char> data;
    if (FILE* f = fopen((g_data_dir + "/cache/pipelines.bin").c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        data.resize((size_t)std::max(0L, ftell(f)));
        fseek(f, 0, SEEK_SET);
        data.resize(fread(data.data(), 1, data.size(), f));
        fclose(f);
    }
    return data;
}
static VkPipelineCache pcache_create(const std::vector<char>& data) {
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    VkPipelineCache pc = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(dev, &ci, nullptr, &pc) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;
        vkCreatePipelineCache(dev, &ci, nullptr, &pc);
    }
    return pc;
}

VkPipelineCache pipeline_cache() {  // the driver checks the header (vendor, device, driver UUID) and ignores stale data
    if (g_pcache) return g_pcache;
    std::vector<char> data = pcache_file();
    g_pcache = pcache_create(data);
    log("vk: pipeline cache: %zu bytes loaded", data.size());
    return g_pcache;
}

std::atomic<uint32_t> g_pipelines_total{0};
void pipeline_cache_save() {
    static std::atomic<bool> busy{false};
    if (!g_pcache || g_pipelines_total == g_pcache_saved_n || busy.exchange(true)) return;
    g_pcache_saved_n = g_pipelines_total;
    // Off the present thread (a 20 MB cache took a frame or more). Ours is merged into a fresh copy of the file, not the
    // file replaced: another run (tests, a second instance) may have saved pipelines this one never loaded, and saving
    // only ours once shrank the file 19.6 -> 10.8 MB, so the next play run stuttered (pipeline creation 6 s in all
    // instead of 0.7). The copy is ours alone (vkMergePipelineCaches needs its destination externally synchronized).
    std::thread([] {
        if (VkPipelineCache disk = pcache_create(pcache_file())) {
            size_t n = 0;
            std::vector<char> data;
            if (vkMergePipelineCaches(dev, disk, 1, &g_pcache) == VK_SUCCESS &&
                vkGetPipelineCacheData(dev, disk, &n, nullptr) == VK_SUCCESS && n) {
                data.resize(n);
                if (vkGetPipelineCacheData(dev, disk, &n, data.data()) != VK_SUCCESS) data.clear();
            }
            vkDestroyPipelineCache(dev, disk, nullptr);
            CreateDirectoryA((g_data_dir + "/cache").c_str(), nullptr);
            std::string path = g_data_dir + "/cache/pipelines.bin", tmp = path + ".tmp";
            if (!data.empty())
                if (FILE* f = fopen(tmp.c_str(), "wb")) {
                    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
                    fclose(f);
                    if (ok) MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
                }
        }
        busy = false;
    }).detach();
}

bool translate(uint32_t stage, const uint8_t* code, uint32_t len, const uint32_t local[3], uint32_t consts_off, uint32_t consts_size, Translated& out, const uint8_t* attr_types) {
    auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(g_sc_lock);
    tcache_open();
    static const uint8_t all_float[32] = {};
    const uint8_t* types = stage == 1 && attr_types ? attr_types : all_float;
    uint32_t in[4] = {stage, consts_off, consts_size, stage == 0 ? local[0] | local[1] << 10 | local[2] << 20 : 0};
    uint64_t prefix = fnv(code, std::min(len, 256u), fnv(types, 32, fnv(in, sizeof in)));
    auto full_hash = [&](uint32_t size) {
        uint64_t h = fnv(code, size);
        return consts_size && consts_off + consts_size <= len ? fnv(code + consts_off, consts_size, h) : h;
    };
    auto [b, e] = g_tcache.equal_range(prefix);
    for (auto it = b; it != e; ++it)
        if (it->second.code_size <= len && full_hash(it->second.code_size) == it->second.full) {
            out = it->second.t;
            g_translate_hits++;
            return true;
        }
    HitchTimer ht{HITCH_TRANSLATE};
    // not in the cache: setup's shaderscan should have made it, so say what kind it is (tools/shaderscan coverage)
    log("vk: shader cache miss: stage %u, %u bytes, consts %u+%u, %s inputs, key %016llx", stage, len, consts_off,
        consts_size, types == all_float || !memcmp(types, all_float, 32) ? "float" : "integer", (unsigned long long)prefix);
    bool ok = translate_sidecar(stage, code, len, local, consts_off, consts_size, out, attr_types);
    if (ok && out.code_size && out.code_size <= len) {
        CacheEntry ce{full_hash(out.code_size), out.code_size, out};
        if (g_tcache_file) {
            uint32_t n = (uint32_t)out.spirv.size(), nres = (uint32_t)out.res.size();
            fwrite(&prefix, 8, 1, g_tcache_file);
            fwrite(&ce.full, 8, 1, g_tcache_file);
            fwrite(&ce.code_size, 4, 1, g_tcache_file);
            fwrite(&n, 4, 1, g_tcache_file);
            fwrite(out.spirv.data(), 4, n, g_tcache_file);
            fwrite(&nres, 4, 1, g_tcache_file);
            fwrite(out.res.data(), sizeof(Resource), nres, g_tcache_file);
            fwrite(&out.frag_outputs, 4, 1, g_tcache_file);
            fflush(g_tcache_file);
        }
        g_tcache.emplace(prefix, std::move(ce));
    }
    g_translate_us += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    g_translate_n++;
    return ok;
}

static bool translate_sidecar(uint32_t stage, const uint8_t* code, uint32_t len, const uint32_t local[3], uint32_t consts_off, uint32_t consts_size, Translated& out, const uint8_t* attr_types) {
#ifdef _WIN32
    if (!g_in_w) spawn();
#else
    if (g_in_w < 0) spawn();
#endif
    uint32_t hdr[3] = {0x4853584E, stage, len};
    wr(hdr, sizeof hdr);
    if (stage == 0) wr(local, 12);
    uint32_t cs[2] = {consts_off, consts_size};
    wr(cs, sizeof cs);
    static const uint8_t all_float[32] = {};
    if (stage == 1) wr(attr_types ? attr_types : all_float, 32);
    wr(code, len);
    if (rdv<uint32_t>() != 0) return false;
    out.code_size = rdv<uint32_t>();
    uint32_t n = rdv<uint32_t>();
    out.spirv.resize(n / 4);
    rd(out.spirv.data(), n);
    uint32_t nres = rdv<uint32_t>();
    out.res.resize(nres);
    for (auto& r : out.res) {
        r.kind = rdv<uint8_t>();
        r.set = rdv<uint8_t>();
        r.binding = rdv<uint16_t>();
        r.slot = rdv<int32_t>();
        r.handle = rdv<int32_t>();
        r.type = rdv<int32_t>();
        r.count = rdv<int32_t>();
    }
    out.frag_outputs = rdv<uint32_t>();
    return true;
}

// NVN program -> Vulkan stages. Stage index: from the Maxwell program header (SPH) after NVN's 0x30-byte header.
ProgramVk* program_of(nvn::Program* p) {
    if (!p) return nullptr;
    if (p->host) return static_cast<ProgramVk*>(p->host);
    // the game sets the same shaders on programs again and again (battle: hundreds per 10 s); share one ProgramVk per
    // shader set, so its combos keep their pipelines. Key: code + control address and the first 512 code bytes per stage.
    static std::unordered_map<std::string, ProgramVk*> by_content;
    std::string ckey;
    for (int i = 0; i < 6; i++) {
        if (!(p->mask & (1u << i))) continue;
        uint64_t k[3] = {p->stages[i].code, p->stages[i].control, fnv(GPTR((uint32_t)p->stages[i].code), 512)};
        ckey.append((const char*)k, sizeof k);
    }
    if (auto it = by_content.find(ckey); it != by_content.end()) { g_programs_shared++; return (ProgramVk*)(p->host = it->second); }
    g_programs_new++;
    {  // measurement: the same code bytes at another address (sharing by content alone would also catch these)
        static std::unordered_set<uint64_t> seen;
        uint64_t h = 0;
        for (int i = 0; i < 6; i++)
            if (p->mask & (1u << i)) h = fnv(GPTR((uint32_t)p->stages[i].code), 512, h ^ i);
        if (!seen.insert(h).second) g_programs_moved++;
    }
    auto* pv = new ProgramVk;
    by_content[ckey] = pv;
    for (int i = 0; i < 6; i++) {
        if (!(p->mask & (1u << i))) continue;
        uint32_t code = (uint32_t)p->stages[i].code;
        // graphics code: NVN header (magic 0x12345678, 0x30 bytes), SPH, instructions. Compute code (measured: control
        // section magic 0x98761234) has neither and starts with the first Maxwell scheduling word.
        bool header = ld32(code) == 0x12345678;
        if (!header && getenv("NX_DUMP_SHADER")) {  // control section, for finding the compute workgroup layout
            std::string path = g_data_dir + "/analysis/ctrl_" + std::to_string(p->stages[i].control) + ".bin";
            if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(GPTR(p->stages[i].control), 1, 0x1000, f); fclose(f); }
        }
        uint32_t sph = header ? ld32(code + 0x30) : 0;
        uint32_t sph_type = sph & 0x1F, shader_type = (sph >> 10) & 0xF;
        uint32_t stage;  // Ryujinx ShaderStage
        if (sph_type == 1 && shader_type >= 1 && shader_type <= 4) stage = shader_type;  // vertex, tess c/e, geometry
        else if (sph_type == 2) stage = 5;                                               // fragment
        else stage = 0;                                                                  // compute (no SPH)
        uint32_t off = header ? 0x30 : 0;
        uint32_t len = 0x20000;
        // compute workgroup size: control section +0x748 (measured: 64, 1, 1 for the light-binning shader)
        uint32_t ctl = p->stages[i].control, local[3] = {1, 1, 1};
        if (stage == 0 && ld32(ctl) == 0x98761234)
            for (int k = 0; k < 3; k++) local[k] = std::max(1u, ld32(ctl + 0x748 + 4 * k));
        // constant data: size at control +0x6fc, offset from the start of the code (incl. NVN header) at +0x700
        uint32_t csize = ld32(ctl + 0x6fc), coff = ld32(ctl + 0x700);
        Translated t;
        if (!translate(stage, (const uint8_t*)GPTR(code + off), len, local, csize ? coff - off : 0, csize, t)) {
            log("vk: shader translation failed (stage %u, code %08x)", stage, code);
            continue;
        }
        // NX_NO_KILL (debug): fragment discard (OpKill / OpTerminateInvocation) becomes OpReturn, to test alpha-test losses
        static const bool no_kill = getenv("NX_NO_KILL") != nullptr;
        if (no_kill && stage == 5)
            for (size_t w = 5; w < t.spirv.size() && (t.spirv[w] >> 16); w += t.spirv[w] >> 16)
                if (t.spirv[w] == 0x000100FCu || t.spirv[w] == 0x00011140u) t.spirv[w] = 0x000100FDu;
        VkShaderModule m = make_module(t);
        static const VkShaderStageFlagBits vks[] = {VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_VERTEX_BIT,
                                                    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                    VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        pv->stages.push_back({vks[stage], m, stage, t, csize ? p->stages[i].code + coff : 0, csize, code + off, csize ? coff - off : 0});
        if (getenv("NX_LOG_PROGRAMS")) {
            std::string rs;
            char b[64];
            for (auto& r : t.res) { snprintf(b, sizeof b, " k%u/s%d/h%d", r.kind, r.slot, r.handle); rs += b; }
            log("vk: program code %08x stage %u: %zu words, consts %u, res%s", (uint32_t)p->stages[i].code, stage, t.spirv.size(), csize, rs.c_str());
        }
        static int nlog;
        if (csize && nlog++ < 8) log("vk: program %08x stage %u: constants at +%#x, %u bytes", p->stages[i].code, stage, coff, csize);
        LOGV("vk: program %08x stage %u: %zu words spirv, %zu resources", p->stages[i].code, stage, t.spirv.size(), t.res.size());
        if (stage == 0)
            for (auto& r : t.res) log("vk: compute %08x local %ux%ux%u: kind %u binding %u slot %d handle %#x", code, local[0], local[1], local[2], r.kind, r.binding, r.slot, r.handle);
    }
    p->host = pv;
    return pv;
}

VkShaderModule vertex_module(const StageVk& st, const uint8_t types[32]) {
    std::string key((const char*)types, 32);
    if (key == std::string(32, '\0')) return st.module;
    auto it = st.variants.find(key);
    if (it != st.variants.end()) return it->second;
    static const uint32_t local[3] = {1, 1, 1};
    Translated t;
    VkShaderModule m = st.module;
    if (translate(1, (const uint8_t*)GPTR(st.src), 0x20000, local, st.src_consts, st.consts_size, t, types)) {
        m = make_module(t);
    } else {
        log("vk: vertex shader %08x: translation with integer attributes failed, keeping float inputs", st.src);
    }
    st.variants[key] = m;
    return m;
}

// Hardware alpha test (NVN ColorState bits 24-27, CommandBuffer SetAlphaRef; Ryujinx bakes it into the shader the same
// way): a fragment module variant that discards before each OpReturn of main unless output 0's alpha passes. The test
// keeps cut-out cards out of the depth pre-pass: without it the battle crowd's transparent card areas hid the rows
// behind them (black stands, only arm tips, user 11-06). func: NVN AlphaFunc 1 never .. 7 gequal.
// shortcut: variants are not in shaders.bin, so recipes cannot prewarm them; they compile on first use (few pipelines).
VkShaderModule alpha_module(const StageVk& st, uint32_t func, float ref) {
    char ks[8] = {'a', (char)func};
    memcpy(ks + 4, &ref, 4);
    const std::string key(ks, 8);  // vertex variants' keys are 32 bytes
    auto it = st.variants.find(key);
    if (it != st.variants.end()) return it->second;
    const std::vector<uint32_t>& in = st.t.spirv;
    uint32_t bound = in[3], entry = 0, out0 = 0, tfloat = 0, tvec4 = 0, tbool = 0;
    std::unordered_map<uint32_t, uint32_t> ptr_output_pointee;
    std::vector<uint32_t> loc0s;
    size_t first_fn = 0;
    for (size_t w = 5; w < in.size();) {
        const uint32_t op = in[w] & 0xFFFF, n = in[w] >> 16;
        if (op == 15) entry = in[w + 2];                                                         // OpEntryPoint
        else if (op == 71 && in[w + 2] == 30 && in[w + 3] == 0) loc0s.push_back(in[w + 1]);      // OpDecorate Location 0
        else if (op == 20) tbool = in[w + 1];                                                    // OpTypeBool
        else if (op == 22 && in[w + 2] == 32) tfloat = in[w + 1];                                // OpTypeFloat 32
        else if (op == 23 && in[w + 2] == tfloat && in[w + 3] == 4) tvec4 = in[w + 1];           // OpTypeVector
        else if (op == 32 && in[w + 2] == 3) ptr_output_pointee[in[w + 1]] = in[w + 3];          // OpTypePointer Output
        else if (op == 59 && in[w + 3] == 3 && std::count(loc0s.begin(), loc0s.end(), in[w + 2]) && ptr_output_pointee[in[w + 1]] == tvec4) out0 = in[w + 2];
        else if (op == 54 && !first_fn) first_fn = w;                                            // OpFunction
        w += n;
    }
    VkShaderModule m = st.module;
    if (out0 && tvec4 && first_fn) {
        static const uint16_t cmp[8] = {0, 184, 184, 180, 188, 186, 182, 190};  // never (a < a), less, equal, lequal, greater, notequal, gequal
        std::vector<uint32_t> o(in.begin(), in.begin() + first_fn);
        const uint32_t cref = bound++;
        uint32_t bits;
        memcpy(&bits, &ref, 4);
        if (!tbool) tbool = bound++, o.push_back(2u << 16 | 20), o.push_back(tbool);
        o.insert(o.end(), {4u << 16 | 43, tfloat, cref, bits});  // OpConstant
        bool in_main = false;
        for (size_t w = first_fn; w < in.size();) {
            const uint32_t op = in[w] & 0xFFFF, n = in[w] >> 16;
            if (op == 54) in_main = in[w + 2] == entry;
            if (op == 253 && in_main) {
                const uint32_t v = bound++, a = bound++, c = bound++, ok = bound++, kill = bound++;
                o.insert(o.end(), {4u << 16 | 61, tvec4, v, out0,                                   // OpLoad
                                   5u << 16 | 81, tfloat, a, v, 3,                                  // OpCompositeExtract
                                   5u << 16 | (uint32_t)cmp[func & 7], tbool, c, a, func == 1 ? a : cref,
                                   3u << 16 | 247, ok, 0,                                           // OpSelectionMerge
                                   4u << 16 | 250, c, ok, kill,                                     // OpBranchConditional
                                   2u << 16 | 248, kill, 1u << 16 | 252,                            // OpLabel, OpKill
                                   2u << 16 | 248, ok});                                            // OpLabel
            }
            o.insert(o.end(), in.begin() + w, in.begin() + w + n);
            w += n;
        }
        o[3] = bound;
        Translated t = st.t;
        t.spirv = std::move(o);
        m = make_module(t);
    } else {
        log("vk: fragment shader %08x has no colour output 0: alpha test func %u ignored", st.src, func);
    }
    st.variants[key] = m;
    return m;
}

uint32_t nvn_stage(uint32_t ryu) {  // Ryujinx ShaderStage -> NVN stage index
    static const uint32_t t[] = {5, 0, 3, 4, 2, 1};
    return ryu < 6 ? t[ryu] : 5;
}

Combo* combo_of(nvn::Program* const per_stage[6]) {
    static std::map<std::vector<ProgramVk*>, Combo*> cache;  // by ProgramVk: a re-set nvn::Program gets new shaders
    std::vector<ProgramVk*> key(6);
    for (int i = 0; i < 6; i++) key[i] = program_of(per_stage[i]);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto* c = new Combo;
    for (int i = 0; i < 6; i++) {
        ProgramVk* pv = key[i];
        if (!pv) continue;
        for (auto& st : pv->stages)
            if (nvn_stage(st.stage) == (uint32_t)i) c->stages.push_back(&st);
    }
    std::vector<std::pair<VkShaderStageFlagBits, const Translated*>> ls;
    for (auto* st : c->stages) ls.push_back({st->vk_stage, &st->t});
    make_layout(ls, &c->set_layout, &c->layout);
    cache[key] = c;
    return c;
}

void make_layout(const std::vector<std::pair<VkShaderStageFlagBits, const Translated*>>& stages, VkDescriptorSetLayout* set_layout, VkPipelineLayout* layout) {
    // one descriptor set: every resource of every stage at its own binding
    std::vector<VkDescriptorSetLayoutBinding> b;
    VkShaderStageFlags all = 0;
    for (auto& st : stages) all |= st.first;
    b.push_back({0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all ? all : VK_SHADER_STAGE_ALL, nullptr});  // support buffer
    for (auto& st : stages)
        for (auto& r : st.second->res) {
            VkDescriptorType ty = r.kind == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                : r.kind == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                : r.kind == 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b.push_back({r.binding, ty, (uint32_t)std::max(1, r.count), (VkShaderStageFlags)st.first, nullptr});
        }
    // One layout pair per binding list (user 11-23): battles load the same shaders at new addresses, so new programs and
    // combos keep coming (~10000 in a 40 battle session), and each new pair kept ~3 MB of driver memory for good: 51 GB
    // private after ~40 minutes. Identical bindings make identical layouts, so the pair is shared (never destroyed).
    static std::mutex lock;
    static std::unordered_map<std::string, std::pair<VkDescriptorSetLayout, VkPipelineLayout>> made;
    std::string key;
    for (auto& x : b) key.append((const char*)&x.binding, 4).append((const char*)&x.descriptorType, 4)
                         .append((const char*)&x.descriptorCount, 4).append((const char*)&x.stageFlags, 4);
    std::lock_guard<std::mutex> l(lock);
    if (auto it = made.find(key); it != made.end()) { *set_layout = it->second.first, *layout = it->second.second; return; }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = (uint32_t)b.size();
    lci.pBindings = b.data();
    VKC(vkCreateDescriptorSetLayout(dev, &lci, nullptr, set_layout));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = set_layout;
    VKC(vkCreatePipelineLayout(dev, &plci, nullptr, layout));
    made.emplace(std::move(key), std::make_pair(*set_layout, *layout));
}

}  // namespace vk
