// nn::audio AudioOut on an SDL3 audio stream. Buffers are released (and the game's event signalled) once SDL has
// consumed them, so the game's audio thread is paced by real playback like on the console.
#include <SDL3/SDL.h>
#include "plat.h"

#include <cmath>
#include <deque>
#include <thread>

#include "rt.h"

using namespace rt;

namespace rt { SDL_Window* ui_window(); void guest_backtrace(const Cpu& c); }

static std::mutex g_au_lock;
static std::deque<uint32_t> g_queued, g_released;  // guest AudioOutBuffer addresses
static uint32_t g_au_event = 0;                    // writable handle of the game's buffer event
static SDL_AudioStream* g_stream;
static bool g_playing = false;
static std::once_flag g_au_once;

// AudioOutBuffer (from the SDK's own setters): data pointer @8, buffer size @0x10, data size @0x18
static void feeder() {
    for (;;) {
        uint32_t b = 0;
        {
            std::lock_guard<std::mutex> l(g_au_lock);
            if (g_playing && !g_queued.empty()) b = g_queued.front();
        }
        if (!b) { Sleep(2); continue; }
        uint32_t data = ld32(b + 8), size = ld32(b + 0x18);
        // every 10 s of audio: buffers, peak and RMS level (the game's and the custom music's), so silence shows
        static uint64_t a_bytes = 0, a_bufs = 0;
        static int a_peak = 0;
        static double a_game2 = 0, a_cust2 = 0;
        {
            const int16_t* s = (const int16_t*)GPTR(data);
            for (uint32_t i = 0; i < size / 2; i++) a_peak = std::max(a_peak, std::abs((int)s[i])), a_game2 += (double)s[i] * s[i];
            a_bufs++;
        }
        static const bool mute = getenv("NX_MUTE") != nullptr;
        const bool silent = !g_stream || mute || g_speed > 1;  // sped up: drop the audio, keep its pace
        if (!silent && size) {  // the custom battle music (ext_bgm.cpp) is mixed into a copy of the game's buffer
            static std::vector<int16_t> mix;
            mix.assign((const int16_t*)GPTR(data), (const int16_t*)GPTR(data) + size / 2);
            custom_bgm_mix(mix.data(), size / 4);
            const int16_t* s = (const int16_t*)GPTR(data);
            for (uint32_t i = 0; i < size / 2; i++) a_cust2 += (double)(mix[i] - s[i]) * (mix[i] - s[i]);
            SDL_PutAudioStreamData(g_stream, mix.data(), (int)size);
        }
        if ((a_bytes += size) >= 48000 * 4 * 10) {
            const double n = a_bytes / 2.0;
            log("audio: %llu buffers in 10 s, peak %d/32767, rms game %.0f custom %.0f", (unsigned long long)a_bufs, a_peak,
                sqrt(a_game2 / n), sqrt(a_cust2 / n));
            a_bytes = a_bufs = 0, a_peak = 0, a_game2 = a_cust2 = 0;
        }
        // keep ~2 buffers queued in SDL: wait until what is left drops below one buffer's worth
        while (g_stream && SDL_GetAudioStreamQueued(g_stream) > (int)size) Sleep(1);
        if (silent) Sleep(size / 192 / g_speed ? size / 192 / g_speed : 1);  // 48 kHz stereo s16 = 192 B/ms
        {
            std::lock_guard<std::mutex> l(g_au_lock);
            g_queued.pop_front();
            g_released.push_back(b);
        }
        if (g_au_event) kevent_signal(g_au_event);
    }
}

// CRI middleware errors: the criErr_Notify family (names.txt) gets (level, message or format, args...); CRI's own
// callback is silent in retail. Formatted with the game's vsnprintf; the Cpu is restored so the probe stays invisible.
static void cri_report(Cpu* c, uint32_t fmt, std::initializer_list<uint32_t> args) {
    static std::atomic<int> n{0};
    if (n++ >= 300) return;
    static thread_local uint32_t buf = hle_alloc(0x200, 4), va = hle_alloc(0x20, 4);
    uint32_t k = 0;
    for (uint32_t a : args) st32(va + 4 * k++, a);
    Cpu saved = *c;
    gcall(c, "game_vsnprintf", {buf, 0x200, fmt, va});
    *c = saved;
    log("[cri] level %u: %s", c->r[0], gstr(buf, 0x200).c_str());
}
PROBE_FN(cri_err0, "criErr_Notify") { cri_report(c, c->r[1], {}); }
PROBE_FN(cri_err_code, "criErr_NotifyCode") { cri_report(c, c->r[1], {}); log("  (code %d)", (int)c->r[2]); }
PROBE_FN(cri_err1, "criErr_Notify1") { cri_report(c, c->r[1], {c->r[2]}); }
PROBE_FN(cri_err2, "criErr_Notify2") { cri_report(c, c->r[1], {c->r[2], c->r[3]}); }
PROBE_FN(cri_err3, "criErr_Notify3") { cri_report(c, c->r[1], {c->r[2], c->r[3], ld32(c->r[13])}); }


HLE_FN(au_Open, "_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE") {
    uint32_t ev = c->r[1], readable;
    g_au_event = kevent_create(&readable);
    attach_system_event(c, ev, readable, true);
    std::call_once(g_au_once, [] {
        ui_window();  // SDL is initialised on the UI thread
        SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!g_stream) log("audio: SDL_OpenAudioDeviceStream failed: %s (continuing silent)", SDL_GetError());
        else SDL_ResumeAudioStreamDevice(g_stream);
        std::thread(feeder).detach();
    });
    log("audio: AudioOut opened (48 kHz, 2 ch, s16)");
    c->r[0] = 0;
}
HLE_FN(au_Start, "_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE") { std::lock_guard<std::mutex> l(g_au_lock); g_playing = true; c->r[0] = 0; }
HLE_FN(au_Stop, "_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE") { std::lock_guard<std::mutex> l(g_au_lock); g_playing = false; }
HLE_FN(au_Close, "_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE") {}
HLE_FN(au_Append, "_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE") {
    std::lock_guard<std::mutex> l(g_au_lock);
    g_queued.push_back(c->r[1]);
    c->r[0] = 1;
}
HLE_FN(au_Released, "_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE") {
    std::lock_guard<std::mutex> l(g_au_lock);
    uint32_t b = 0;
    if (!g_released.empty()) { b = g_released.front(); g_released.pop_front(); }
    c->r[0] = b;
}
HLE_FN(au_Rate, "_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE") { c->r[0] = 48000; }
HLE_FN(au_Channels, "_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE") { c->r[0] = 2; }
HLE_FN(au_Format, "_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE") { c->r[0] = 2; }  // PcmInt16
HLE_FN(au_Name, "_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE") {
    static uint32_t name = [] { uint32_t a = hle_alloc(16); memcpy(GPTR(a), "DeviceOut", 10); return a; }();
    c->r[0] = name;
}
HLE_FN(au_DeviceNotif, "_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE") {
    uint32_t readable;
    kevent_create(&readable);  // never signalled: the output device does not change
    attach_system_event(c, c->r[0], readable, true);
    c->r[0] = 0;
}
