// Custom battle music (setting random_battle_bgm = 2 custom / 3 mixed; user request 10-30): CRI ADX files from a
// folder (setting custom_bgm_dir, default <data>\music) play instead of the game's battle track, and the victory theme
// that matches the battle track's series plays at the victory. Made for the P5R "random battle music" mod's loose
// files: "<id>_<series>_<title>.adx", e.g. "1_P5R_Last Surprise Battle.adx" -> "300_P5R_Victory.adx".
// - pairing: a title with "Victory" is a victory theme, everything else a battle track. The victory for a battle
//   series: the same series; else the one with the longest common start if one series starts the other or 9+
//   characters are the same ("Persona 2 InnocentSin" -> "Persona 2 EternalPunishment"); "P4" = "Persona 4"; else a
//   random victory theme.
// - decoder (our own): ADX v4, 4-bit ADPCM, 18-byte frames x channels (32 samples), 2-tap predictor from the header's
//   highpass frequency, scale = (16-bit BE & 0x1fff) + 1; encryption type 9 (flag 0x13 = 9): the scale is XORed with an
//   LCG stepped once per frame in file order (keycode from setting custom_bgm_key, none built in: encrypted files without
//   it are skipped with a log line); loop points from the v4 header
//   (+0x18 + 4 ch: align, 1, loop flag, start sample, start byte, end sample, end byte). At the loop start the decoder
//   state (history + XOR) is saved and restored at the loop end.
// - output: mixed into the game's own audio buffers (hle_audio.cpp feeder, 48 kHz stereo s16); 1 s fade out when the
//   game's music takes over again. Other rates are resampled linearly. Loudness: each track is scaled to the RMS
//   level of the game's own battle music in its output (BGM_RMS), times setting custom_bgm_volume / 7 (0..10, default
//   7 = the game's level). Measured 11-01: the game's BGM files (Common.awb, plain ADX) have RMS 3400..6400 (quartiles)
//   and come out ~0.175 x (field music ~750 in the audio log); battle tracks are the loud ones -> ~1100. The P5R files
//   are 4800..12300, so the old fixed 0.7 gain was 10..18 dB louder than the game (user 11-01: far too loud).
#include "plat.h"

#include <filesystem>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rt.h"

using namespace rt;

namespace {
struct Track { std::string path, series; bool victory; };
std::vector<Track> g_tracks;
std::once_flag g_scan_once;

uint32_t rnd() {
    static uint32_t s = (uint32_t)GetTickCount() * 2654435761u | 1;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return s;
}
std::string lower(std::string s) {
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}
std::string folder() {
    const char* d = getenv("NX_CUSTOM_BGM_DIR");
#ifdef _WIN32
    return d && *d ? std::string(d) : g_data_dir + "\\music";
#else
    return d && *d ? std::string(d) : g_data_dir + "/music";
#endif
}
#ifndef _WIN32
void scan() {  // Linux: every *.adx (any case) in the folder, sorted
    const std::string dir = folder();
    std::vector<std::string> names;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 4 && lower(n.substr(n.size() - 4)) == ".adx") names.push_back(n);
    }
    if (names.empty()) { log("custom music: no .adx files in %s", dir.c_str()); return; }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string stem = name.substr(0, name.size() - 4);
        const size_t a = stem.find('_'), b = a == std::string::npos ? a : stem.find('_', a + 1);
        Track t{dir + "/" + name, b == std::string::npos ? "" : stem.substr(a + 1, b - a - 1),
                lower(stem).find("victory") != std::string::npos};
        if (t.series == "P4") t.series = "Persona 4";
        g_tracks.push_back(t);
    }
    int v = 0;
    for (auto& t : g_tracks) v += t.victory;
    log("custom music: %zu battle tracks, %d victory themes in %s", g_tracks.size() - v, v, dir.c_str());
}
#else
void scan() {
    const std::string dir = folder();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.adx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { log("custom music: no .adx files in %s", dir.c_str()); return; }
    do {
        const std::string name = fd.cFileName, stem = name.substr(0, name.size() - 4);
        const size_t a = stem.find('_'), b = a == std::string::npos ? a : stem.find('_', a + 1);
        Track t{dir + "\\" + name, b == std::string::npos ? "" : stem.substr(a + 1, b - a - 1),
                lower(stem).find("victory") != std::string::npos};
        if (t.series == "P4") t.series = "Persona 4";
        g_tracks.push_back(t);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    int v = 0;
    for (auto& t : g_tracks) v += t.victory;
    log("custom music: %zu battle tracks, %d victory themes in %s", g_tracks.size() - v, v, dir.c_str());
}
#endif
const Track* pick_battle() {
    std::vector<const Track*> v;
    for (auto& t : g_tracks) if (!t.victory) v.push_back(&t);
    return v.empty() ? nullptr : v[rnd() % v.size()];
}
const Track* pick_victory(const std::string& series) {
    const Track* best = nullptr;
    size_t best_n = 0;
    std::vector<const Track*> all;
    for (auto& t : g_tracks) {
        if (!t.victory) continue;
        all.push_back(&t);
        if (t.series == series) return &t;
        size_t n = 0;
        while (n < series.size() && n < t.series.size() && series[n] == t.series[n]) n++;
        const bool ok = n >= 9 || (n && n == std::min(series.size(), t.series.size()));
        if (ok && n > best_n) best = &t, best_n = n;
    }
    return best ? best : all.empty() ? nullptr : all[rnd() % all.size()];
}

// ------------------------------------------------------------------------------------------------ ADX decoder
struct Adx {
    std::vector<uint8_t> f;
    uint32_t ch = 0, rate = 0, frame = 0, data = 0, total = 0;
    bool enc = false, loop = false;
    uint32_t loop_start = 0, loop_end = 0;  // samples
    int c1 = 0, c2 = 0;
    uint16_t mul = 0, add = 0;
    // decoder state
    struct State { uint32_t pos; uint16_t xr; int h[2][2]; } st{}, at_loop{};
    int16_t buf[2][32];
    uint32_t buf_frame = ~0u;  // frame index in buf
    bool open(const std::string& path) {
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        f.resize((size_t)ftell(fp));
        fseek(fp, 0, SEEK_SET);
        const bool rd = fread(f.data(), 1, f.size(), fp) == f.size();
        fclose(fp);
        auto be16 = [&](size_t o) { return (uint32_t)f[o] << 8 | f[o + 1]; };
        auto be32 = [&](size_t o) { return be16(o) << 16 | be16(o + 2); };
        if (!rd || f.size() < 0x40 || be16(0) != 0x8000 || f[4] != 3 || f[6] != 4 || f[7] < 1 || f[7] > 2) return false;
        frame = f[5], ch = f[7], rate = be32(8), total = be32(12), data = be16(2) + 4;
        if (frame != 18 || !rate || data >= f.size()) return false;
        enc = f[0x13] == 9;
        if (f[0x12] == 4) {
            const size_t p = 0x18 + 4 * ch + (ch == 1 ? 4 : 0);
            if (p + 24 <= data && be32(p + 4)) loop = true, loop_start = be32(p + 8), loop_end = be32(p + 16);
        }
        if (loop_end > total || loop_start >= loop_end) loop = false;
        const double hp = be16(0x10), a = std::sqrt(2.0) - std::cos(2 * 3.14159265358979 * hp / rate), b = std::sqrt(2.0) - 1;
        const double c = (a - std::sqrt((a + b) * (a - b))) / b;
        c1 = (int)(c * 8192), c2 = (int)(c * c * -4096);
        static const uint64_t keycode = [] { const char* e = getenv("NX_CUSTOM_BGM_KEY"); return e ? strtoull(e, nullptr, 10) : 0ull; }();
        if (enc && !keycode) { log("custom music: %s is encrypted: set custom_bgm_key in settings.ini (or use decrypted files)", path.c_str()); return false; }
        const uint64_t k = keycode - 1;
        st = {};
        st.xr = (uint16_t)((k >> 27) & 0x7fff), mul = (uint16_t)(((k >> 12) & 0x7ffc) | 1), add = (uint16_t)(((k << 1) & 0x7fff) | 1);
        if (!enc && keycode) {  // some repacked files have flag 0 but P5R-keyed frames (40_SMT3_Fiend Battle): keyed scales come out far smaller
            uint64_t plain = 0, keyed = 0;
            uint16_t xr = st.xr;
            for (size_t i = 0, o = data; i < 64 && o + 2 <= f.size(); i++, o += frame)
                plain += be16(o) & 0x1fff, keyed += (be16(o) ^ xr) & 0x1fff, xr = (uint16_t)((xr * mul + add) & 0x7fff);
            enc = keyed * 4 < plain;
        }
        return true;
    }
    // decode frame index fi (sequential: st.pos is the next frame)
    void decode_next() {
        const uint32_t fi = st.pos;
        if (loop && fi == loop_start / 32) at_loop = st;
        for (uint32_t c = 0; c < ch; c++) {
            const size_t o = data + ((size_t)fi * ch + c) * frame;
            if (o + frame > f.size()) { memset(buf[c], 0, sizeof buf[c]); continue; }
            uint32_t raw = (uint32_t)f[o] << 8 | f[o + 1];
            if (enc) raw ^= st.xr, st.xr = (uint16_t)((st.xr * mul + add) & 0x7fff);
            const int scale = (int)(raw & 0x1fff) + 1;
            int h1 = st.h[c][0], h2 = st.h[c][1];
            for (int i = 0; i < 32; i++) {
                int n = (f[o + 2 + i / 2] >> (i & 1 ? 0 : 4)) & 15;
                if (n >= 8) n -= 16;
                int s = n * scale + ((c1 * h1 + c2 * h2) >> 12);
                s = std::clamp(s, -32768, 32767);
                h2 = h1, h1 = s;
                buf[c][i] = (int16_t)s;
            }
            st.h[c][0] = h1, st.h[c][1] = h2;
        }
        buf_frame = fi;
        st.pos++;
    }
    // sample n, both channels, called with n = 0, 1, 2, ... (n jumps back to the loop start at the loop end);
    // false at the end (no loop)
    bool sample(uint64_t& n, int& l, int& r) {
        if (loop && n >= loop_end) n = loop_start, st = at_loop, buf_frame = ~0u;
        if (n >= total) return false;
        const uint32_t fi = (uint32_t)(n / 32);
        while (fi != buf_frame) decode_next();  // sequential: the next frame (or the loop start frame again)
        l = buf[0][n % 32], r = buf[ch - 1][n % 32];
        return true;
    }
};

// ------------------------------------------------------------------------------------------------ player
std::mutex g_lock;
constexpr double BGM_RMS = 1100;  // the game's music in its audio output (s16 RMS; see the header)
struct Voice {
    Adx adx;
    uint64_t pos = 0;  // next sample to read
    int cur[2] = {}, next[2] = {};  // samples pos - 2 and pos - 1: the output lies between them at frac
    bool primed = false, done = false;
    double frac = 0;
    float fade = 1, fade_step = 0, norm = 0;  // norm: gain to the game's music level
    std::string series;
    bool read(int* s) {
        if (!adx.sample(pos, s[0], s[1])) return false;
        pos++;
        return true;
    }
};
std::shared_ptr<Voice> g_voice;  // playing (g_lock)
std::string g_battle_series;     // series of the battle track playing (victory pairing)
bool g_active = false;           // a custom battle or victory track owns the music (g_lock)

void start(const Track* t, bool battle) {
    if (!t) return;
    auto v = std::make_shared<Voice>();
    const std::string path = t->path, series = t->series;
    std::thread([v, path, series, battle] {  // file load off the game thread
        if (!v->adx.open(path)) { log("custom music: cannot read %s", path.c_str()); return; }
        double sum = 0;  // RMS of the first 60 s, on a copy (the decoder is sequential)
        uint64_t n = 0;
        {
            Adx probe = v->adx;
            int a, b;
            for (uint64_t k = 0; n < 60ull * probe.rate && probe.sample(k, a, b); k++, n++) sum += (double)a * a + (double)b * b;
        }
        const double rms = n ? std::sqrt(sum / (2.0 * n)) : 0;
        v->norm = rms > 1 ? (float)std::min(1.0, BGM_RMS / rms) : 1.0f;
        std::lock_guard<std::mutex> l(g_lock);
        if (!g_active) return;  // the game took the music back meanwhile
        v->series = series;
        g_voice = v;
        log("custom music: %s %s (%u Hz, %u ch, loop %s, rms %.0f, gain %.3f)", battle ? "battle" : "victory", path.c_str(),
            v->adx.rate, v->adx.ch, v->adx.loop ? "yes" : "no", rms, v->norm);
    }).detach();
}
}  // namespace

int rt::custom_bgm_mode() {
    const char* e = getenv("NX_RANDOM_BATTLE_BGM");
    return e ? std::clamp(atoi(e), 0, 3) : 0;
}
// bgm_play hook (hle_sys.cpp): a battle track starts. True = custom music plays (the game's track is to be stopped).
bool rt::custom_bgm_battle() {
    const int mode = custom_bgm_mode();
    if (mode < 2) return false;
    std::call_once(g_scan_once, scan);
    if (mode == 3 && rnd() % 2) return false;  // mixed: half the battles keep a game track
    const Track* t = pick_battle();
    if (!t) return false;
    std::lock_guard<std::mutex> l(g_lock);
    g_active = true, g_battle_series = t->series;
    if (g_voice) g_voice->fade_step = 1.0f / 4800;  // a battle track still fading: 0.1 s
    start(t, true);
    return true;
}
// the victory jingle (bgm 65) while custom battle music plays: the matching victory theme. True = handled.
bool rt::custom_bgm_victory() {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_active) return false;
    if (g_voice) g_voice->fade_step = 1.0f / 4800;
    start(pick_victory(g_battle_series), false);
    return true;
}
// the game's music takes over (another track on the slot, the field music restored): fade out over 1 s
void rt::custom_bgm_stop(const char* why) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_active) return;
    g_active = false;
    if (g_voice && !g_voice->fade_step) g_voice->fade_step = 1.0f / 48000;
    log("custom music: stop (%s)", why);
}
bool rt::custom_bgm_active() { std::lock_guard<std::mutex> l(g_lock); return g_active; }

// hle_audio.cpp feeder: add the custom music to n stereo s16 frames at 48 kHz
void rt::custom_bgm_mix(int16_t* out, uint32_t n) {
    std::shared_ptr<Voice> v;
    {
        std::lock_guard<std::mutex> l(g_lock);
        v = g_voice;
    }
    if (!v) return;
    const char* ve = getenv("NX_CUSTOM_BGM_VOLUME");
    const float gain = (ve ? std::clamp(atoi(ve), 0, 10) : 7) / 7.0f * v->norm;
    const double step = (double)v->adx.rate / 48000;
    if (!v->primed) {
        v->primed = true;
        if (!v->read(v->cur) || !v->read(v->next)) v->done = true;
    }
    for (uint32_t i = 0; i < n && !v->done; i++) {
        const float t = (float)v->frac, g = gain * v->fade;
        for (int c = 0; c < 2; c++) {
            const float x = (v->cur[c] + (v->next[c] - v->cur[c]) * t) * g;
            out[2 * i + c] = (int16_t)std::clamp((int)(out[2 * i + c] + x), -32768, 32767);
        }
        for (v->frac += step; v->frac >= 1 && !v->done; v->frac -= 1) {
            v->cur[0] = v->next[0], v->cur[1] = v->next[1];
            if (!v->read(v->next)) v->done = true;
        }
        if (v->fade_step && (v->fade -= v->fade_step) <= 0) v->done = true;
    }
    if (v->done) v->fade = 0;
    if (v->fade <= 0) {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_voice == v) g_voice = nullptr;
    }
}
