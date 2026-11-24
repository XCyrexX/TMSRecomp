// Host window, input and display timing (SDL3), plus the nn::vi and nn::hid hooks that read them.
// SDL wants its events pumped on the thread that made the window, so a dedicated UI thread owns both.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include "plat.h"
#ifdef _WIN32
#include <timeapi.h>
#include <psapi.h>
#include <intrin.h>
#endif

#include <algorithm>
#include <thread>

#include "rt.h"

namespace nvn { void vk_wait_present_taken(double max_ms); }  // vk.cpp: the previous frame went to the display path (back-pressure)
namespace nvn { extern std::atomic<int> g_seq_request; }  // vk_replay.cpp: NX_CONTROL "seq"

namespace rt {

struct PadState {
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};

static std::mutex g_ui_lock;
static PadState g_pad;
static SDL_Window* g_window;
static std::vector<uint32_t> g_vsync_events;  // writable kernel event handles
static std::once_flag g_ui_once;
static std::condition_variable g_ui_ready_cv;
static bool g_ui_ready = false;
std::atomic<uint64_t> g_vsync_count{0};
std::atomic<uint64_t> g_pace_tick{0};  // vsync tick at which the last frame was released (present on the next tick)
std::atomic<uint64_t> g_late_vsyncs{0};  // vsyncs missed because a frame was not ready (frame stats)
std::atomic<uint32_t> g_frame_work_max_us{0};  // longest game frame (pace return to next pace call), frame stats
std::atomic<uint64_t> g_frame_no{0};           // pace_present calls (NX_PROFILE spike frames)
static std::condition_variable g_vsync_cv;
// wait until the vsync clock reaches tick (present_thread: fixed display cadence)
void wait_vsync_tick(uint64_t tick) {
    std::unique_lock<std::mutex> l(g_ui_lock);
    g_vsync_cv.wait(l, [&] { return g_vsync_count >= tick; });
}

// NVN present interval n: the frame stays on screen for n vblanks of the 60 Hz clock, so the next present waits for
// the n-th vblank after the previous one. The game asks for 2 (30 fps). NX_PRESENT_INTERVAL=n overrides it (1 = try
// 60 fps); NX_NO_VSYNC skips pacing (benchmarks).
// NX_FPS=0 / N > 60: no vblank pacing; wait for the cap (if any), then give the next game frame the measured time of
// this one as its dt (at most 1/30 s: slower frames slow the game down, as on the Switch).
static std::atomic<int> g_fixed_dt{0};
static void pace_variable(int fps) {
    static const uint64_t freq = SDL_GetPerformanceFrequency();
    static uint64_t prev = 0;
    // never make frames faster than they reach the screen (bounded: 2 frame times of the cap, 60 fps uncapped)
    if (!g_fixed_dt) nvn::vk_wait_present_taken(2000.0 / (fps ? fps : 60));
    uint64_t now = SDL_GetPerformanceCounter();
    if (prev) {  // uncapped still stops at 1000 fps: dt is clamped to >= 1 ms, faster frames would speed the game up
        uint64_t target = prev + freq / (fps ? fps : 1000);
        while (now < target) {
            if (target - now > freq / 500) Sleep(1);  // sleep until ~2 ms before, then spin
            else YieldProcessor();
            now = SDL_GetPerformanceCounter();
        }
    }
    // debug NX_CONTROL "fixdt n": every frame steps 1/n s whatever its real time (slow drawlog / capture frames keep
    // the rate); "fixdt 0" ends it
    const int fixed = g_fixed_dt;
    if (prev) set_game_dt(fixed ? 1.0f / fixed : std::clamp((float)(now - prev) / freq, 1 / 1000.0f, 1 / 30.0f));
    prev = now;
}

// NX_STALL_STATS=1 (diagnostic, 1% low work s85): every 10 s, for the slowest 1% of game frames (pace to pace) and for the
// median ones: wall ms, ms the game thread really ran (QueryThreadCycleTime), process CPU ms (all threads) and page faults.
// Running ~= wall: the slow frame is more work; running << wall: the thread was not running (lock, preemption, kernel).
static void stall_stats() {
#ifdef _WIN32
    static const bool on = getenv("NX_STALL_STATS") != nullptr;
    if (!on) return;
    struct F { float wall, run, proc, pf; };
    extern std::atomic<uint64_t> g_ww_reset_pages, g_commit_pages;
    static uint64_t ww0 = 0, cm0 = 0;
    static std::vector<F> v;
    static uint64_t q0 = 0, c0 = 0, p0 = 0, t0 = 0, pf0 = 0, win_q = 0, win_t = 0;
    static LARGE_INTEGER fq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t c = 0, p = 0, t = __rdtsc();
    QueryThreadCycleTime(GetCurrentThread(), &c);
    QueryProcessCycleTime(GetCurrentProcess(), &p);
    PROCESS_MEMORY_COUNTERS pm{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    static double tsc_per_ms = 0;
    if (q0) {
        double wall = (double)(q.QuadPart - q0) * 1000.0 / fq.QuadPart;
        if (tsc_per_ms == 0) tsc_per_ms = (double)(t - t0) / wall;
        v.push_back({(float)wall, (float)((c - c0) / tsc_per_ms), (float)((p - p0) / tsc_per_ms), (float)(pm.PageFaultCount - pf0)});
    } else win_q = q.QuadPart, win_t = t;
    q0 = q.QuadPart, c0 = c, p0 = p, t0 = t, pf0 = pm.PageFaultCount;
    if ((double)(q.QuadPart - win_q) / fq.QuadPart >= 10 && v.size() > 200) {
        tsc_per_ms = (double)(t - win_t) * fq.QuadPart / 1000.0 / (double)(q.QuadPart - win_q);  // recalibrate on the window
        std::sort(v.begin(), v.end(), [](const F& a, const F& b) { return a.wall > b.wall; });
        auto avg = [&](size_t from, size_t n) { F s{}; for (size_t i = from; i < from + n; i++) s.wall += v[i].wall, s.run += v[i].run, s.proc += v[i].proc, s.pf += v[i].pf;
                                                 return F{s.wall / n, s.run / n, s.proc / n, s.pf / n}; };
        size_t k = std::max<size_t>(v.size() / 100, 1);
        F s = avg(0, k), m = avg(v.size() / 2 - k / 2, k);
        uint64_t ww = g_ww_reset_pages.load();
        uint64_t cm = g_commit_pages.load();
        log("stall stats: %.0f write-watch pages re-armed, %.0f guest pages committed per frame", (double)(ww - ww0) / v.size(), (double)(cm - cm0) / v.size());
        ww0 = ww, cm0 = cm;
        log("stall stats: slowest 1%% (%zu frames): %.2f ms wall, game thread ran %.2f ms, process CPU %.2f ms, %.0f page faults; median: %.2f ms wall, ran %.2f, process %.2f, %.0f faults; worst %.2f wall ran %.2f faults %.0f",
            k, s.wall, s.run, s.proc, s.pf, m.wall, m.run, m.proc, m.pf, v[0].wall, v[0].run, v[0].pf);
        v.clear(), win_q = q.QuadPart, win_t = t;
    }
#endif  // NX_STALL_STATS: Windows only (thread / process cycle counters)
}

std::atomic<uint64_t> g_pace_us{0};  // time inside pace_present (cap / present waits), frame stats
static std::atomic<int> g_refresh_hz{0};  // the window's display at start (0 = unknown)
// present_mode = vrr (G-SYNC / FreeSync, user 11-23): FIFO (vk.cpp) and, when the frame rate is uncapped or above the
// display, a cap 3 fps under its refresh so every frame stays inside the VRR range (no vsync queue, no tearing).
static int vrr_cap(int fps) {
    static const bool vrr = getenv("NX_PRESENT_MODE") && !strcmp(getenv("NX_PRESENT_MODE"), "vrr");
    const int hz = g_refresh_hz;
    if (!vrr || hz < 50 || (fps > 0 && fps <= std::max(60, hz - 3))) return fps;  // 30 / 60 keep their own pacing
    static bool said = false;
    if (!said) said = true, log("ui: VRR: frame cap %d (display %d Hz)", hz - 3, hz);
    return hz - 3;
}
void pace_present(uint32_t interval) {
    const int fps = vrr_cap(fps_setting());
    stall_stats();
    frame_begin();
    g_frame_no++;
    profile_start();
    if (fps == 0 || fps > 60) {
        const uint64_t t0 = SDL_GetPerformanceCounter();
        pace_variable(fps);
        g_pace_us += (SDL_GetPerformanceCounter() - t0) * 1000000 / SDL_GetPerformanceFrequency();
        return;
    }
    static uint64_t last = 0;
    static const bool off = getenv("NX_NO_VSYNC") != nullptr;
    static const char* force = getenv("NX_PRESENT_INTERVAL");
    if (force) interval = (uint32_t)atoi(force);
    if (off || !interval) return;
    static uint64_t back = 0;  // performance counter when the previous pace returned
    uint64_t in = SDL_GetPerformanceCounter();
    if (back) {
        uint32_t us = (uint32_t)((in - back) * 1000000 / SDL_GetPerformanceFrequency());
        if (us > g_frame_work_max_us) g_frame_work_max_us = us;
    }
    std::unique_lock<std::mutex> l(g_ui_lock);
    uint64_t target = last + interval;
    if (g_vsync_count < target) g_vsync_cv.wait(l, [&] { return g_vsync_count >= target; });
    else if (g_vsync_count > target) g_late_vsyncs += g_vsync_count - target;  // the frame took too long
    last = g_vsync_count;
    g_pace_tick = last;
    l.unlock();
    back = SDL_GetPerformanceCounter();
}

// Npad button bits (nn::hid::NpadButton)
enum : uint64_t { B_A = 1 << 0, B_B = 1 << 1, B_X = 1 << 2, B_Y = 1 << 3, B_LS = 1 << 4, B_RS = 1 << 5, B_L = 1 << 6,
                  B_R = 1 << 7, B_ZL = 1 << 8, B_ZR = 1 << 9, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12,
                  B_UP = 1 << 13, B_RIGHT = 1 << 14, B_DOWN = 1 << 15 };

static uint64_t button_mask(const std::string& b) {
    if (size_t plus = b.find('+'); plus != std::string::npos && plus > 0) {  // "l3+r3": together
        uint64_t x = button_mask(b.substr(0, plus)), y = button_mask(b.substr(plus + 1));
        return x && y ? x | y : 0;
    }
    return b == "l3" ? B_LS : b == "r3" ? B_RS : b == "a" ? B_A : b == "b" ? B_B : b == "x" ? B_X : b == "y" ? B_Y : b == "plus" ? B_PLUS : b == "minus" ? B_MINUS
         : b == "up" ? B_UP : b == "down" ? B_DOWN : b == "left" ? B_LEFT : b == "right" ? B_RIGHT : b == "l" ? B_L
         : b == "r" ? B_R : b == "zl" ? B_ZL : b == "zr" ? B_ZR : 0;
}

// NX_CONTROL=<file>: live control for an agent driving the game. Each line appended to the file is a command: a button
// name (pressed for 150 ms; several lines in one write press together) or "shot" (the next presented frame is written
// to <data>/shots/live_NNNNN.bmp), "drawlog" (log every draw of the next frame, like NX_LOG_DRAWS) or "rt" (dump the
// colour targets of the next frame, like NX_DUMP_RT) or "speed n" (run the game n times faster; 1 = normal) or
// "ls x y ms" / "rs x y ms" (hold the left / right stick at x, y in -32767..32767 for ms game milliseconds) or
// "verbose n" (like -v: 1 logs fs reads etc., 0 stops) or "cover" (NX_COVER report now) or "flag n [0|1]" (log or
// set game flag n, hle_sys.cpp) or "pos" (log the player position) or "poke addr byte" (hex, debug write).
std::atomic<int> g_shot_request{0}, g_drawlog_request{0}, g_rt_request{0}, g_user_shot{0};  // g_user_shot: F12
static int g_ctl_stick[4];  // NX_CONTROL lx ly rx ry, valid until g_ctl_stick_until
static uint64_t g_ctl_stick_until[2];
static uint64_t control_buttons(uint64_t now) {
    static const char* path = getenv("NX_CONTROL");
    static long done = 0;  // bytes consumed
    static uint64_t next_check = 0, until = 0, held = 0;
    if (!path) return 0;
    if (now >= next_check) {
        next_check = now + 50;
        if (FILE* f = fopen(path, "rb")) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            if (size > done) {
                std::string buf((size_t)(size - done), ' ');
                fseek(f, done, SEEK_SET);
                buf.resize(fread(buf.data(), 1, buf.size(), f));
                size_t end = buf.rfind('\n');
                if (end != std::string::npos) {
                    done += (long)end + 1;
                    size_t pos = 0;
                    while (pos <= end) {
                        size_t nl = buf.find('\n', pos);
                        std::string cmd = buf.substr(pos, nl - pos);
                        while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
                        if (cmd == "shot") g_shot_request++;
                        else if (cmd.rfind("shots ", 0) == 0) g_shot_request += atoi(cmd.c_str() + 6);  // the next N frames
                        else if (cmd.rfind("drawlog", 0) == 0) g_drawlog_request += cmd.size() > 8 ? atoi(cmd.c_str() + 8) : 1;  // "drawlog N": N frames
                        else if (cmd == "rt") g_rt_request++;
                        else if (cmd.rfind("skipshadow ", 0) == 0) rt::g_skip_shadow = atoi(cmd.c_str() + 11);
                        else if (cmd.rfind("aa ", 0) == 0) rt::apply_aa(atoi(cmd.c_str() + 3));
                        else if (cmd.rfind("upq ", 0) == 0) rt::apply_fsr3_quality(atoi(cmd.c_str() + 4));
                        else if (cmd.rfind("msaa ", 0) == 0) rt::apply_msaa(atoi(cmd.c_str() + 5));
                        else if (cmd == "cover") cover_report();
                        else if (cmd.rfind("speed ", 0) == 0) set_speed(atoi(cmd.c_str() + 6));
                        else if (cmd.rfind("env ", 0) == 0) { _putenv(cmd.c_str() + 4); log("env %s", cmd.c_str() + 4); }  // env NX_KEY=v: a setting, live
                        else if (cmd.rfind("fps ", 0) == 0) set_fps(atoi(cmd.c_str() + 4));
                        else if (cmd.rfind("verbose ", 0) == 0) g_verbose = atoi(cmd.c_str() + 8);
                        else if (cmd.rfind("flag ", 0) == 0) flag_command(cmd.c_str() + 5);
                        else if (cmd.rfind("fixdt ", 0) == 0) { g_fixed_dt = atoi(cmd.c_str() + 6); log("fixdt %d", g_fixed_dt.load()); }
                        else if (cmd.rfind("dtkeep ", 0) == 0) { g_dt_keep = strtoul(cmd.c_str() + 7, nullptr, 16); log("dtkeep %08x", g_dt_keep.load()); }
                        else if (cmd == "pos") pos_command();
                        else if (cmd == "lmap") rt::overlay_request_map();
                        else if (cmd == "seq") nvn::g_seq_request = 1;
                        else if (cmd.rfind("travel ", 0) == 0) rt::overlay_request_travel(atoi(cmd.c_str() + 7));
                        else if (cmd == "actors") pos_command("all");
                        else if (cmd.rfind("uipart ", 0) == 0) { g_ui_part30 = strtoul(cmd.c_str() + 7, nullptr, 16); log("uipart %x", g_ui_part30.load()); }
                        else if (cmd.rfind("gate30 ", 0) == 0) { g_gate30 = strtoul(cmd.c_str() + 7, nullptr, 16); log("gate30 %x", g_gate30.load()); }
                        else if (cmd.rfind("uifreeze ", 0) == 0) { g_ui_freeze = atoi(cmd.c_str() + 9); log("uifreeze %u", g_ui_freeze.load()); }
                        else if (cmd.rfind("uigate ", 0) == 0) { g_ui_gate = atoi(cmd.c_str() + 7); log("uigate %d", g_ui_gate.load()); }
                        else if (cmd.rfind("setpos ", 0) == 0) pos_command(cmd.c_str() + 7);
                        else if (cmd.rfind("watch", 0) == 0) watch_command(cmd.c_str() + 5);
                        else if (cmd.rfind("dump ", 0) == 0) dump_command(cmd.c_str() + 5);
                        else if (cmd.rfind("task30 ", 0) == 0) rt::task30_command(cmd.c_str() + 7);
                        else if (cmd.rfind("uidraw30 ", 0) == 0) rt::g_uidraw30 = (uint32_t)strtoul(cmd.c_str() + 9, nullptr, 16);
                        else if (cmd.rfind("poke ", 0) == 0) {  // poke addr byte (hex): one guest byte, debug
                            unsigned pa = 0, pv = 0;
                            if (sscanf(cmd.c_str() + 5, "%x %x", &pa, &pv) == 2) { log("poke %08x: %02x -> %02x", pa, ld8(pa), pv); st8(pa, pv); }
                        }
                        else if (cmd.rfind("ls ", 0) == 0 || cmd.rfind("rs ", 0) == 0) {
                            int s = cmd[0] == 'r', x = 0, y = 0, ms = 0;
                            sscanf(cmd.c_str() + 3, "%d %d %d", &x, &y, &ms);
                            g_ctl_stick[2 * s] = x;
                            g_ctl_stick[2 * s + 1] = y;
                            g_ctl_stick_until[s] = now + ms / g_speed;
                        }
                        else if (uint64_t m = button_mask(cmd)) { held |= m; until = now + 150 / g_speed; }
                        else if (!cmd.empty()) log("NX_CONTROL: unknown command '%s'", cmd.c_str());
                        pos = nl + 1;
                    }
                }
            }
            fclose(f);
        }
    }
    if (now >= until) held = 0;
    return held;
}

// NX_INPUT="5000:plus,8000:a,8500:a": press buttons for 150 ms at these times (ms after start), for unattended runs
// (milestone 3 replaces this with recorded input replay)
static uint64_t scripted_buttons() {
    static std::vector<std::pair<uint64_t, uint64_t>> script = [] {
        std::vector<std::pair<uint64_t, uint64_t>> v;
        const char* s = getenv("NX_INPUT");
        if (!s) return v;
        std::string str(s);
        size_t pos = 0;
        while (pos < str.size()) {
            size_t comma = str.find(',', pos);
            std::string item = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t colon = item.find(':');
            if (colon != std::string::npos) {
                uint64_t t = strtoull(item.c_str(), nullptr, 10);
                v.push_back({t, button_mask(item.substr(colon + 1))});
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return v;
    }();
    static uint64_t t0 = SDL_GetTicks();
    uint64_t now = SDL_GetTicks() - t0, m = 0;
    for (auto& [t, b] : script)
        if (now >= t && now < t + 150) m |= b;
    m |= control_buttons(now);
    for (int s = 0; s < 2; s++)
        if (now >= g_ctl_stick_until[s]) g_ctl_stick[2 * s] = g_ctl_stick[2 * s + 1] = 0;
    return m;
}

static void poll_input() {
    PadState p;
    p.buttons |= scripted_buttons();
    const bool* k = SDL_GetKeyboardState(nullptr);
    struct { SDL_Scancode sc; uint64_t b; } keys[] = {
        {SDL_SCANCODE_K, B_A}, {SDL_SCANCODE_RETURN, B_A}, {SDL_SCANCODE_J, B_B}, {SDL_SCANCODE_BACKSPACE, B_B},
        {SDL_SCANCODE_I, B_X}, {SDL_SCANCODE_U, B_Y}, {SDL_SCANCODE_Q, B_L}, {SDL_SCANCODE_E, B_R},
        {SDL_SCANCODE_1, B_ZL}, {SDL_SCANCODE_3, B_ZR}, {SDL_SCANCODE_SPACE, B_PLUS}, {SDL_SCANCODE_TAB, B_MINUS},
        {SDL_SCANCODE_LEFT, B_LEFT}, {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_RIGHT, B_RIGHT}, {SDL_SCANCODE_DOWN, B_DOWN}};
    for (auto& m : keys)
        if (k[m.sc]) p.buttons |= m.b;
    p.lx = g_ctl_stick[0], p.ly = g_ctl_stick[1], p.rx = g_ctl_stick[2], p.ry = g_ctl_stick[3];
    if (k[SDL_SCANCODE_A]) p.lx = -32767;
    if (k[SDL_SCANCODE_D]) p.lx = 32767;
    if (k[SDL_SCANCODE_W]) p.ly = 32767;
    if (k[SDL_SCANCODE_S]) p.ly = -32767;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    static SDL_Gamepad* pad = nullptr;
    // replays (agent test runs) never read the real pad: SDL hands it to background windows, and the user's DualSense
    // (touchpad = battle speed, L3+R3 = F1 menu) changed settings and quit test runs (11-01)
    static const bool replay = getenv("NX_REPLAY") && *getenv("NX_REPLAY");
    if (pad && !SDL_GamepadConnected(pad)) SDL_CloseGamepad(pad), pad = nullptr;  // unplugged: take the next one
    if (!pad && n > 0 && !replay && (pad = SDL_OpenGamepad(ids[0]))) {
        SDL_GamepadType t = SDL_GetGamepadType(pad);
        g_pad_kind = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 1
                     : t >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ? 0
                     : 2;  // Xbox, and generic pads (mostly XInput)
        log("input: pad \"%s\" (type %d) -> %s button icons", SDL_GetGamepadName(pad), (int)t,
            g_pad_kind == 1 ? "PlayStation" : g_pad_kind == 2 ? "Xbox" : "Switch");
    }
    SDL_free(ids);
    if (pad) {
        // positional mapping: the face button on the right is A, as on a Switch controller
        struct { SDL_GamepadButton gb; uint64_t b; } btn[] = {
            {SDL_GAMEPAD_BUTTON_EAST, B_A}, {SDL_GAMEPAD_BUTTON_SOUTH, B_B}, {SDL_GAMEPAD_BUTTON_NORTH, B_X},
            {SDL_GAMEPAD_BUTTON_WEST, B_Y}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, B_LS}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_RS},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R},
            {SDL_GAMEPAD_BUTTON_START, B_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, B_MINUS},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN}};
        for (auto& m : btn)
            if (SDL_GetGamepadButton(pad, m.gb)) p.buttons |= m.b;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) p.buttons |= B_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) p.buttons |= B_ZR;
        auto ax = [&](SDL_GamepadAxis a, bool inv) {
            int v = SDL_GetGamepadAxis(pad, a);
            if (v > -4000 && v < 4000) v = 0;  // deadzone
            v = inv ? -v : v;
            return v < -32767 ? -32767 : v > 32767 ? 32767 : v;
        };
        if (int v = ax(SDL_GAMEPAD_AXIS_LEFTX, false)) p.lx = v;
        if (int v = ax(SDL_GAMEPAD_AXIS_LEFTY, true)) p.ly = v;
        if (int v = ax(SDL_GAMEPAD_AXIS_RIGHTX, false)) p.rx = v;
        if (int v = ax(SDL_GAMEPAD_AXIS_RIGHTY, true)) p.ry = v;
    }
    // battle speed 1x -> 2x -> 3x -> 1x: F3 or the DualSense touchpad (a button the Switch game never sees)
    static bool speed_prev = false;
    const bool speed = k[SDL_SCANCODE_F3] || (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD));
    if (speed && !speed_prev) set_battle_speed(battle_speed() % 3 + 1, true);
    speed_prev = speed;
    static bool f1_prev = false;
    bool f1 = k[SDL_SCANCODE_F1] && !f1_prev;
    f1_prev = k[SDL_SCANCODE_F1];
#ifdef _WIN32
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    void* hwnd = g_window;  // Linux: the menu is drawn into the frame (overlay.cpp), it only needs the window size
#endif
    if (hwnd && overlay_update(hwnd, p.buttons, p.ly, f1)) p = PadState{};
    p.buttons |= overlay_buttons();
    std::lock_guard<std::mutex> l(g_ui_lock);
    g_pad = p;
}

void ui_set_fullscreen(bool on) { SDL_SetWindowFullscreen(g_window, on); }

// The game's 60 Hz vsync clock (nn::vi vsync events, pace_present). Its own thread on an absolute schedule with a
// spin wait (see below), and SDL event pumping cannot delay it.
// The swapchain does not block (mailbox, vk.cpp), so this clock alone sets the frame times.
static void vsync_thread() {
    // Windows 11 power throttling (EcoQoS) coalesces timers and slows threads of a window that is not in front: the
    // clock fired up to 36 ms late. Opt the process and this thread out (no-op on older Windows).
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE pp{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    BOOL ok1 = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pp, sizeof pp);
    THREAD_POWER_THROTTLING_STATE tp{THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
    BOOL ok2 = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof tp);
    log("vsync: power throttling opt-out process %d thread %d (error %lu)", ok1, ok2, GetLastError());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    const uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t base = SDL_GetPerformanceCounter(), k = 0;  // tick k is due at base + k * freq / (60 * speed), no drift
    int speed = g_speed;
    for (;;) {
        if (g_speed != speed) { speed = g_speed; base = SDL_GetPerformanceCounter(); k = 0; }
        uint64_t next = base + (uint64_t)((double)++k * freq / (60.0 * speed));
        uint64_t now = SDL_GetPerformanceCounter();
        if (now > next + freq / 10) { base = now; k = 0; next = now; }  // fell far behind (debugger, sleep): resync
        if (next > now) {
            // Spin (one core busy, ticks exact to ~0.1 ms, measured). Sleeping is unreliable: with the window in the
            // background Windows ignores the 1 ms timer resolution and the high-resolution waitable timer and Sleep(1)
            // both overslept by up to 20-36 ms (measured 2026-10-23), even with power throttling turned off.
            // Sleep(1) until 3 ms before the tick, then spin: saves the core, fine in the foreground. Default at fps 0 / > 60,
            // where this clock does not pace frames: the full spin at time-critical priority slowed the game and render
            // threads whenever it shared a physical core with one (s83: Shibuya 1% low 84 -> 107 fps without it).
            // NX_VSYNC_SPIN=1 / 0 forces the full spin / the sleep at every fps.
            static const char* const spin_env = getenv("NX_VSYNC_SPIN");
            const int fps = rt::fps_setting();
            const bool spin = spin_env ? strcmp(spin_env, "0") != 0 : fps == 30 || fps == 60;
            const uint64_t margin = freq * 3 / 1000;
            while (!spin && SDL_GetPerformanceCounter() + margin < next) Sleep(1);
            while (SDL_GetPerformanceCounter() < next) YieldProcessor();
        }
        uint64_t t0 = SDL_GetPerformanceCounter();
        std::vector<uint32_t> ev;
        {
            std::lock_guard<std::mutex> l(g_ui_lock);
            g_vsync_count++;
            ev = g_vsync_events;
        }
        uint64_t t1 = SDL_GetPerformanceCounter();
        g_vsync_cv.notify_all();
        for (uint32_t h : ev) kevent_signal(h);
        uint64_t t2 = SDL_GetPerformanceCounter();
        {  // clock health every 600 ticks: how late ticks fired, time in the ui lock / kernel event signal
            static double late = 0, lock = 0, sig = 0;
            static uint32_t n = 0;
            auto ms = [&](uint64_t a, uint64_t b) { return (double)(b - a) * 1000.0 / freq; };
            late = std::max(late, t0 > next ? ms(next, t0) : 0.0);
            lock = std::max(lock, ms(t0, t1));
            sig = std::max(sig, ms(t1, t2));
            if (++n == 600) {
                log("vsync clock: max late %.2f ms, ui lock %.2f ms, event signal %.2f ms", late, lock, sig);
                n = 0; late = lock = sig = 0;
            }
        }
    }
}

static void ui_thread() {

    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");  // the exe icon (resource 1) when built with one
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) fatal("SDL_Init: %s", SDL_GetError());
    g_window = SDL_CreateWindow("Tokyo Mirage Sessions #FE Encore (TMSRecomp)", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    // <data>/icon.ico: the launcher makes it from the player's own game (NSP icon or title screen); none = default icon
#ifdef _WIN32
    if (HWND hw = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        const std::string ico = g_data_dir + "/icon.ico";
        HICON big = (HICON)LoadImageA(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
        HICON sml = (HICON)LoadImageA(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
        if (big) SendMessageW(hw, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (sml) SendMessageW(hw, WM_SETICON, ICON_SMALL, (LPARAM)sml);
    }
#endif
    // NX_WINDOW=borderless: borderless fullscreen on the window's display (play.bat default); Alt+Enter or F11 toggles
    if (const char* w = getenv("NX_WINDOW"); w && !strcmp(w, "borderless")) SDL_SetWindowFullscreen(g_window, true);
    {
        std::lock_guard<std::mutex> l(g_ui_lock);
        g_ui_ready = true;
    }
    g_ui_ready_cv.notify_all();
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)))
        log("ui: display %dx%d @ %.3f Hz", m->w, m->h, m->refresh_rate), g_refresh_hz = (int)(m->refresh_rate + 0.5f);
    std::thread(vsync_thread).detach();
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                log("window closed");
                fflush(nullptr);
#ifdef _WIN32
                ExitProcess(0);
#else
                _exit(0);  // as ExitProcess: no static destructors while the game threads still run
#endif
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F12) g_user_shot++;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {  // no mouse cursor with a pad
                int n = 0;
                SDL_free(SDL_GetGamepads(&n));
                n ? SDL_HideCursor() : SDL_ShowCursor();
            }
        }
        poll_input();
        Sleep(1);
    }
}

void ui_start() {
    std::call_once(g_ui_once, [] { std::thread(ui_thread).detach(); });
    std::unique_lock<std::mutex> l(g_ui_lock);
    g_ui_ready_cv.wait(l, [] { return g_ui_ready; });
}

SDL_Window* ui_window() {
    ui_start();
    return g_window;
}

}  // namespace rt

using namespace rt;

// ------------------------------------------------------------------------------------------------ nn::vi
HLE_FN(vi_Initialize, "_ZN2nn2vi10InitializeEv") { ui_start(); }
HLE_FN(vi_Finalize, "_ZN2nn2vi8FinalizeEv") {}
HLE_FN(vi_OpenDisplay, "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE") {
    log("vi: OpenDefaultDisplay");
    st32(c->r[0], hle_alloc(64));
    c->r[0] = 0;
}
HLE_FN(vi_CloseDisplay, "_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE") {}
HLE_FN(vi_CreateLayer, "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE") {
    st32(c->r[0], hle_alloc(64));
    c->r[0] = 0;
}
HLE_FN(vi_DestroyLayer, "_ZN2nn2vi12DestroyLayerEPNS0_5LayerE") {}
HLE_FN(vi_GetNativeWindow, "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE") {
    st32(c->r[0], hle_alloc(64));  // opaque to the game, handed to nvnWindowBuilderSetNativeWindow
    c->r[0] = 0;
}
HLE_FN(vi_VsyncEvent, "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE") {
    uint32_t ev = c->r[0], readable;
    log("vi: vsync event -> %08x", ev);
    uint32_t w = kevent_create(&readable);
    attach_system_event(c, ev, readable, true);
    log("vi: vsync event %08x state %u", ev, ld8(ev + 0x1c));
    {
        std::lock_guard<std::mutex> l(g_ui_lock);
        g_vsync_events.push_back(w);
    }
    c->r[0] = 0;
}

// ------------------------------------------------------------------------------------------------ nn::hid
static uint64_t g_sampling = 0, g_press_read = ~0ull;  // pad reads so far; the last read with a new button press
std::atomic<uint64_t> rt::g_pad_game_buttons{0};
bool rt::pad_pressed_now() { return g_press_read == g_sampling; }
HLE_FN(hid_InitNpad, "_ZN2nn3hid14InitializeNpadEv") { ui_start(); }
HLE_FN(hid_SetStyles, "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {}
HLE_FN(hid_SetIds, "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjj") {}
HLE_FN(hid_SetHold, "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE") {}
HLE_FN(hid_ShowSupport, "_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE") { c->r[0] = 0; }
HLE_FN(hid_GetStyle, "_ZN2nn3hid15GetNpadStyleSetERKj") {
    c->r[0] = ld32(c->r[0]) == 0 ? 1u : 0u;  // player 1 = full key (Pro Controller); nothing else connected
}
// Input record / replay (milestone 3), keyed by the game's pad reads (one per game frame), not by wall time, so a replay
// follows the game's own pace. NX_RECORD=<file>: one line per change, "<read> <buttons hex> <lx> <ly> <rx> <ry>".
// NX_REPLAY=<file>: the game reads the recorded state instead of live input until the file ends, then live input
// resumes. Reads during loading depend on load speed, so long replays can drift (measure before relying on one).
static void replay_record(uint64_t n, PadState& p) {
    struct Entry { uint64_t n; PadState p; };
    static std::vector<Entry> replay = [] {
        std::vector<Entry> v;
        const char* f = getenv("NX_REPLAY");
        FILE* fp = f ? fopen(f, "r") : nullptr;
        if (f && !fp) log("input: cannot open replay %s", f);
        if (!fp) return v;
        Entry e;
        unsigned long long b;
        while (fscanf(fp, "%llu %llx %d %d %d %d", &e.n, &b, &e.p.lx, &e.p.ly, &e.p.rx, &e.p.ry) == 6) {
            e.p.buttons = b;
            v.push_back(e);
        }
        fclose(fp);
        log("input: replaying %zu changes from %s (last at read %llu)", v.size(), f, v.empty() ? 0ull : (unsigned long long)v.back().n);
        return v;
    }();
    static size_t next = 0;
    static PadState cur;
    if (next < replay.size() || (!replay.empty() && n == replay.back().n + 1)) {
        while (next < replay.size() && replay[next].n <= n) cur = replay[next++].p;
        if (n % 300 == 0) log("input: replay at read %llu", (unsigned long long)n);  // for cutting a replay short
        if (next == replay.size() && n > replay.back().n) log("input: replay finished at read %llu, live input from here", (unsigned long long)n);
        else p = cur;
    }
    static FILE* rec = [] {
        const char* f = getenv("NX_RECORD");
        FILE* fp = f ? fopen(f, "w") : nullptr;
        if (f) log(fp ? "input: recording to %s" : "input: cannot write %s", f);
        return fp;
    }();
    static PadState last{~0ull};
    if (rec && (p.buttons != last.buttons || p.lx != last.lx || p.ly != last.ly || p.rx != last.rx || p.ry != last.ry)) {
        fprintf(rec, "%llu %llx %d %d %d %d\n", (unsigned long long)n, (unsigned long long)p.buttons, p.lx, p.ly, p.rx, p.ry);
        fflush(rec);  // survives a crash
        last = p;
    }
}

static void npad_state(Cpu* c, bool connected) {
    uint32_t out = c->r[0];
    memset(GPTR(out), 0, 0x28);
    if (!connected) return;
    PadState p;
    {
        std::lock_guard<std::mutex> l(g_ui_lock);
        p = g_pad;
    }
    replay_record(++g_sampling, p);
    static uint64_t prev = 0;  // a new press at this read: UI widgets update now even between 30 Hz ticks (hle_sys.cpp)
    if (p.buttons & ~prev) g_press_read = g_sampling;
    prev = p.buttons;
    st64(out, g_sampling);
    st64(out + 8, p.buttons);
    g_pad_game_buttons = p.buttons;
    st32(out + 16, (uint32_t)p.lx);
    st32(out + 20, (uint32_t)p.ly);
    st32(out + 24, (uint32_t)p.rx);
    st32(out + 28, (uint32_t)p.ry);
    st32(out + 32, 0x3);  // IsConnected | IsWired
}
HLE_FN(hid_FullKey, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj") { npad_state(c, ld32(c->r[1]) == 0); }
HLE_FN(hid_Handheld, "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj") { npad_state(c, false); }
HLE_FN(hid_JoyDual, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj") { npad_state(c, false); }
HLE_FN(hid_JoyLeft, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj") { npad_state(c, false); }
HLE_FN(hid_JoyRight, "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj") { npad_state(c, false); }
HLE_FN(hid_InitTouch, "_ZN2nn3hid21InitializeTouchScreenEv") {}
HLE_FN(hid_Touch, "_ZN2nn3hid19GetTouchScreenStateILj16EEEvPNS0_16TouchScreenStateIXT_EEE") {
    memset(GPTR(c->r[0]), 0, 16);  // sampling number, count 0
}
HLE_FN(hid_SixAxisHandles, "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {
    for (uint32_t i = 0; i < c->r[1]; i++) st32(c->r[0] + 4 * i, 0x5A000000u | i);
    c->r[0] = c->r[1];
}
HLE_FN(hid_SixAxisState, "_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE") {
    memset(GPTR(c->r[0]), 0, 0x60);
}
HLE_FN(hid_VibHandles, "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {
    for (uint32_t i = 0; i < c->r[1]; i++) st32(c->r[0] + 4 * i, 0x56000000u | i);
    c->r[0] = c->r[1];
}
HLE_FN(hid_VibInit, "_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE") {}
HLE_FN(hid_VibSend, "_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE") {}
