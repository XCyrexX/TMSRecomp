#pragma once
// Portability. Windows: <windows.h>. Linux: POSIX versions of the few small Win32 helpers the runtime calls in many
// places (sleep, yield, environment, case-insensitive compare), so that code reads the same on both. Larger
// platform-specific parts (memory, threads, files, crash handler, drawing) have their own #ifdef _WIN32 blocks.
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include <sched.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

inline void Sleep(unsigned ms) {
    timespec t{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
inline int SwitchToThread() { return sched_yield() == 0; }
#define YieldProcessor() __builtin_ia32_pause()
inline int _putenv_s(const char* k, const char* v) { return setenv(k, v, 1); }
inline int _putenv(const char* kv) {  // "K=V" sets, "K=" removes (as on Windows)
    const char* eq = strchr(kv, '=');
    if (!eq || eq == kv) return -1;
    std::string k(kv, eq - kv);
    return eq[1] ? setenv(k.c_str(), eq + 1, 1) : unsetenv(k.c_str());
}
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
inline uint64_t GetTickCount64() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
inline uint32_t GetTickCount() { return (uint32_t)GetTickCount64(); }
inline bool CreateDirectoryA(const char* p, void*) { return mkdir(p, 0777) == 0; }
inline bool DeleteFileA(const char* p) { return unlink(p) == 0; }
constexpr unsigned MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
inline bool MoveFileExA(const char* from, const char* to, unsigned flags) {  // rename always replaces
    if (flags & MOVEFILE_WRITE_THROUGH) {  // the data on disk before the name points at it
        FILE* f = fopen(from, "rb");
        if (f) fsync(fileno(f)), fclose(f);
    }
    return rename(from, to) == 0;
}
constexpr uint32_t INVALID_FILE_ATTRIBUTES = 0xFFFFFFFFu;
inline uint32_t GetFileAttributesA(const char* p) {  // only "exists" and "is a folder" (0x10) are used
    struct stat st;
    return stat(p, &st) ? INVALID_FILE_ATTRIBUTES : S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
}
constexpr unsigned INFINITE = 0xFFFFFFFFu;  // Sleep(INFINITE): about 49 days, callers loop
[[noreturn]] inline void ExitProcess(unsigned code) { _exit((int)code); }  // as on Windows: no static destructors

namespace plat {
// the running program (Windows: GetModuleFileName)
inline std::filesystem::path exe_path() {
    char b[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
    return n > 0 ? std::filesystem::path(std::string(b, (size_t)n)) : std::filesystem::path("tms");
}
// settings.ini: NX_INI, else next to the program when that folder is writable (a folder install, like Windows), else
// $XDG_CONFIG_HOME/TMSRecomp (an AppImage or a system install, whose own folder is read-only)
inline std::filesystem::path settings_ini() {
    if (const char* e = getenv("NX_INI"); e && *e) return e;
    const std::filesystem::path dir = exe_path().parent_path();
    if (access(dir.c_str(), W_OK) == 0 && !getenv("APPIMAGE")) return dir / "settings.ini";
    std::filesystem::path cfg;
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) cfg = x;
    else cfg = std::filesystem::path(getenv("HOME") ? getenv("HOME") : ".") / ".config";
    std::error_code ec;
    std::filesystem::create_directories(cfg / "TMSRecomp", ec);
    return cfg / "TMSRecomp" / "settings.ini";
}
}  // namespace plat
#endif
