/* Guest CPU state and helpers used by the recompiled code (C11 and C++).
 * Guest memory is one 4 GiB window at a fixed host address, so a guest address is just an offset. */
#pragma once
#ifdef NX_GAME_DLL
/* game.dll (the recompiled code) is built on the player's PC with only clang + lld-link (tools/setup/build_game.py):
 * no C library headers. Runtime entry points are imported from tms.exe, the few C library functions the code calls
 * from the CRT DLLs tms.exe already uses (import libs made from tools/setup/nx_*.def). Declarations match the MSVC headers. */
typedef __INT8_TYPE__ int8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT32_TYPE__ int32_t;
typedef __INT64_TYPE__ int64_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT32_TYPE__ uint32_t;
typedef __UINT64_TYPE__ uint64_t;
typedef __SIZE_TYPE__ size_t;
#define INT32_MIN (-__INT32_MAX__ - 1)
#define INT32_MAX __INT32_MAX__
#define UINT32_MAX __UINT32_MAX__
#define INT64_MIN (-__INT64_MAX__ - 1)
#define INT64_MAX __INT64_MAX__
#define UINT64_MAX __UINT64_MAX__
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define memcpy __builtin_memcpy
double sqrt(double);
float sqrtf(float);
double floor(double);
float floorf(float);
double ceil(double);
float ceilf(float);
double round(double);
float roundf(float);
double trunc(double);
float truncf(float);
double nearbyint(double);
float nearbyintf(float);
double fabs(double);
float fabsf(float);
double fmin(double, double);
float fminf(float, float);
double fmax(double, double);
float fmaxf(float, float);
#ifdef _WIN32
typedef struct __declspec(align(16)) { unsigned long long Part[2]; } _SETJMP_FLOAT128;
typedef _SETJMP_FLOAT128 jmp_buf[16];
int __cdecl _setjmp(jmp_buf);
#define setjmp _setjmp
#define NX_API __declspec(dllimport)
#else
/* Linux (game.so): glibc's x86-64 jmp_buf (struct __jmp_buf_tag[1], 200 bytes); _setjmp does not save the signal mask.
 * The C library functions resolve against the libc / libm that tms already loaded. */
typedef long long jmp_buf[25];
int _setjmp(jmp_buf);
#define setjmp _setjmp
#define NX_API __attribute__((visibility("default")))
#endif
#else
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define NX_API __declspec(dllexport) /* tms.exe exports what game.dll calls */
#else
#define NX_API __attribute__((visibility("default"))) /* tms (linked with -rdynamic) exports what game.so calls */
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GUEST_BASE 0x10000000000ull /* 1 TiB: reserved at startup with VirtualAlloc at this exact address */

typedef struct Cpu {
    uint32_t r[16];
    uint8_t n, z, cf, v;           /* APSR flags as 0/1 */
    uint8_t q, fn, fz, fc, fv;     /* sticky Q, FPSCR NZCV */
    uint8_t excl_valid, excl_size, pad0;
    uint32_t ge;                   /* GE[3:0] */
    uint32_t fpscr;                /* control bits only; NZCV live in fn..fv */
    uint32_t sh_carry;             /* carry out of the last register-shifted operand */
    uint32_t tpidruro, tpidrurw;   /* TLS registers (cp15 c13) */
    uint32_t excl_addr;
    uint64_t excl_val;
    union {
        float f[64];
        uint32_t w[64];
        double d[32];
        uint64_t x[32];
    } vr;                          /* S0-S31 alias D0-D15; D16-D31; Qn = D2n:D2n+1 */
    void* host;                    /* runtime thread object */
    uint32_t hook_bypass;          /* hook slot + 1 whose next entry runs the guest code (call_unhooked), 0 = none */
} Cpu;

typedef void (*GuestFn)(Cpu*);
typedef struct { uint32_t addr; GuestFn fn; } GuestFunc;
typedef struct { const char* name; uint32_t addr; } GuestHookName;
typedef struct {
    const char* name;
    uint32_t base, size, text_lo, text_hi, ro_lo, ro_hi, data_lo, data_hi, init;
} GuestModule;

#ifdef NX_GAME_DLL
/* defined in table.c; tms.exe reaches them (and the other tables) through game.dll's exports (dispatch.cpp game_load) */
#ifdef _WIN32
extern GuestFn g_hooks[];
extern GuestFn g_probes[];  /* observers of names-file functions: run at entry, then the function itself */
#else
/* game.so is compiled with -fvisibility=hidden (direct calls between the generated files, no PLT); these declarations
 * give table.c's definitions default visibility, so they are the library's only exports (dispatch.cpp game_load) */
#define NX_TABLE extern __attribute__((visibility("default")))
NX_TABLE GuestFn g_hooks[];
NX_TABLE GuestFn g_probes[];  /* observers of names-file functions: run at entry, then the function itself */
NX_TABLE const GuestFunc g_guest_funcs[];
NX_TABLE const uint32_t g_guest_func_count;
NX_TABLE const GuestHookName g_hook_names[];
NX_TABLE const uint32_t g_hook_count;
NX_TABLE const GuestHookName g_hook_aliases[];
NX_TABLE const uint32_t g_hook_alias_count;
NX_TABLE const GuestHookName g_thunks[];
NX_TABLE const uint32_t g_thunk_count;
NX_TABLE const GuestModule g_modules[];
NX_TABLE const uint32_t g_module_count;
NX_TABLE const uint32_t g_image_size;
#undef NX_TABLE
#endif
#endif

/* runtime entry points (runtime/src) */
NX_API void dispatch(Cpu* c, uint32_t target);
NX_API void guest_trap(Cpu* c, uint32_t addr, const char* why);
NX_API void guest_unimpl(Cpu* c, uint32_t addr, uint32_t word);
NX_API void guest_bad_branch(Cpu* c, uint32_t from, uint32_t to);
NX_API void guest_noreturn(Cpu* c, uint32_t addr);
NX_API void guest_svc(Cpu* c, uint32_t imm, uint32_t addr);
NX_API void guest_yield(Cpu* c);
NX_API uint32_t cp15_read(Cpu* c, int opc1, int crn, int crm, int opc2);
NX_API void cp15_write(Cpu* c, int opc1, int crn, int crm, int opc2, uint32_t v);

NX_API uint64_t guest_counter(void);  /* CNTPCT, 19.2 MHz */

/* setjmp/longjmp: host jump buffer + saved guest callee-saved state, keyed by the guest jmp_buf address.
 * The host side is the C runtime's setjmp: it restores all non-volatile registers (locals such as `c` survive),
 * and longjmp unwinds the recompiled frames between (they carry unwind info). */
typedef struct GuestJmp {
    jmp_buf host;
    uint32_t r[16];
    uint64_t d[8];          /* d8-d15 */
    uint32_t val;
} GuestJmp;
NX_API GuestJmp* guest_setjmp_slot(Cpu* c, uint32_t guest_buf);
NX_API void guest_setjmp_save(Cpu* c, GuestJmp* j);
NX_API void guest_setjmp_resume(Cpu* c, GuestJmp* j);

#define HOOK(i) do { if (g_hooks[i]) { if (c->hook_bypass == (i) + 1u) c->hook_bypass = 0; else { g_hooks[i](c); return; } } } while (0)
NX_API extern uint32_t* g_cover;  /* NX_COVER: call counts per hook slot */
#define PROBE(i) do { if (g_cover) g_cover[i]++; if (g_probes[i]) g_probes[i](c); } while (0)
#define JUMP(t) do { if ((t) == ret0) return; dispatch(c, (t)); return; } while (0)

/* ------------------------------------------------------------------------------------------------- memory */
#define GPTR(a) ((uint8_t*)(GUEST_BASE + (uint64_t)(uint32_t)(a)))
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, GPTR(a), 4); return v; }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, GPTR(a), 2); return v; }
static inline uint32_t ld8(uint32_t a) { return *GPTR(a); }
static inline uint32_t lds16(uint32_t a) { int16_t v; memcpy(&v, GPTR(a), 2); return (uint32_t)(int32_t)v; }
static inline uint32_t lds8(uint32_t a) { return (uint32_t)(int32_t)(int8_t)*GPTR(a); }
static inline uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, GPTR(a), 8); return v; }
static inline void st32(uint32_t a, uint32_t v) { memcpy(GPTR(a), &v, 4); }
static inline void st16(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(GPTR(a), &h, 2); }
static inline void st8(uint32_t a, uint32_t v) { *GPTR(a) = (uint8_t)v; }
static inline void st64(uint32_t a, uint64_t v) { memcpy(GPTR(a), &v, 8); }

/* exclusive monitor: a store-exclusive succeeds if memory still holds what the load-exclusive saw (CAS) */
#define EXCL_LOAD(N, T)                                                                                     \
    static inline uint32_t excl_load##N(Cpu* c, uint32_t a) {                                             \
        T v = __atomic_load_n((T*)GPTR(a), __ATOMIC_SEQ_CST);                                             \
        c->excl_addr = a; c->excl_val = v; c->excl_valid = 1; c->excl_size = N;                           \
        return (uint32_t)v;                                                                               \
    }                                                                                                     \
    static inline uint32_t excl_store##N(Cpu* c, uint32_t a, uint32_t v) {                                \
        if (!c->excl_valid || c->excl_addr != a || c->excl_size != N) { c->excl_valid = 0; return 1; }    \
        c->excl_valid = 0;                                                                                \
        T expect = (T)c->excl_val;                                                                        \
        return __atomic_compare_exchange_n((T*)GPTR(a), &expect, (T)v, 0, __ATOMIC_SEQ_CST,               \
                                           __ATOMIC_SEQ_CST) ? 0 : 1;                                     \
    }
EXCL_LOAD(8, uint8_t)
EXCL_LOAD(16, uint16_t)
EXCL_LOAD(32, uint32_t)
static inline uint64_t excl_load64(Cpu* c, uint32_t a) {
    uint64_t v = __atomic_load_n((uint64_t*)GPTR(a), __ATOMIC_SEQ_CST);
    c->excl_addr = a; c->excl_val = v; c->excl_valid = 1; c->excl_size = 64;
    return v;
}
static inline uint32_t excl_store64(Cpu* c, uint32_t a, uint64_t v) {
    if (!c->excl_valid || c->excl_addr != a || c->excl_size != 64) { c->excl_valid = 0; return 1; }
    c->excl_valid = 0;
    uint64_t expect = c->excl_val;
    return __atomic_compare_exchange_n((uint64_t*)GPTR(a), &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
}
/* LDA/STL (plain acquire / release, ARMv8) */
static inline uint32_t acq_load8(uint32_t a) { return __atomic_load_n(GPTR(a), __ATOMIC_SEQ_CST); }
static inline uint32_t acq_load16(uint32_t a) { return __atomic_load_n((uint16_t*)GPTR(a), __ATOMIC_SEQ_CST); }
static inline uint32_t acq_load32(uint32_t a) { return __atomic_load_n((uint32_t*)GPTR(a), __ATOMIC_SEQ_CST); }
static inline void rel_store8(uint32_t a, uint32_t v) { __atomic_store_n(GPTR(a), (uint8_t)v, __ATOMIC_SEQ_CST); }
static inline void rel_store16(uint32_t a, uint32_t v) { __atomic_store_n((uint16_t*)GPTR(a), (uint16_t)v, __ATOMIC_SEQ_CST); }
static inline void rel_store32(uint32_t a, uint32_t v) { __atomic_store_n((uint32_t*)GPTR(a), v, __ATOMIC_SEQ_CST); }
static inline uint32_t atomic_swap32(uint32_t a, uint32_t v) { return __atomic_exchange_n((uint32_t*)GPTR(a), v, __ATOMIC_SEQ_CST); }
static inline uint32_t atomic_swap8(uint32_t a, uint32_t v) { return __atomic_exchange_n(GPTR(a), (uint8_t)v, __ATOMIC_SEQ_CST); }

/* ------------------------------------------------------------------------------------------------- integer */
static inline uint32_t ror32(uint32_t v, unsigned s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline void nz(Cpu* c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }
static inline uint32_t add_flags(Cpu* c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t s = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)s;
    c->cf = (uint8_t)(s >> 32);
    c->v = ((a ^ r) & (b ^ r)) >> 31;
    c->n = r >> 31;
    c->z = r == 0;
    return r;
}
static inline uint32_t sh_lsl(uint32_t v, uint32_t s) { s &= 0xff; return s < 32 ? v << s : 0; }
static inline uint32_t sh_lsr(uint32_t v, uint32_t s) { s &= 0xff; return s < 32 ? v >> s : 0; }
static inline uint32_t sh_asr(uint32_t v, uint32_t s) { s &= 0xff; return (uint32_t)((int32_t)v >> (s < 32 ? s : 31)); }
static inline uint32_t sh_ror(uint32_t v, uint32_t s) { return ror32(v, s & 31); }
static inline uint32_t sh_lsl_c(Cpu* c, uint32_t v, uint32_t s) {
    s &= 0xff;
    if (s == 0) { c->sh_carry = c->cf; return v; }
    c->sh_carry = s <= 32 ? (v >> (32 - s)) & 1 : 0;
    return s < 32 ? v << s : 0;
}
static inline uint32_t sh_lsr_c(Cpu* c, uint32_t v, uint32_t s) {
    s &= 0xff;
    if (s == 0) { c->sh_carry = c->cf; return v; }
    c->sh_carry = s <= 32 ? (v >> (s - 1)) & 1 : 0;
    return s < 32 ? v >> s : 0;
}
static inline uint32_t sh_asr_c(Cpu* c, uint32_t v, uint32_t s) {
    s &= 0xff;
    if (s == 0) { c->sh_carry = c->cf; return v; }
    if (s >= 32) { c->sh_carry = v >> 31; return (uint32_t)((int32_t)v >> 31); }
    c->sh_carry = (v >> (s - 1)) & 1;
    return (uint32_t)((int32_t)v >> s);
}
static inline uint32_t sh_ror_c(Cpu* c, uint32_t v, uint32_t s) {
    s &= 0xff;
    if (s == 0) { c->sh_carry = c->cf; return v; }
    uint32_t r = ror32(v, s & 31);
    c->sh_carry = r >> 31;
    return r;
}
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t rbit32(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
    return __builtin_bswap32(v);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (b == 0) return 0;
    if (a == 0x80000000u && b == 0xffffffffu) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint32_t usad8(uint32_t a, uint32_t b) {
    uint32_t s = 0;
    for (int i = 0; i < 32; i += 8) {
        int x = (a >> i) & 0xff, y = (b >> i) & 0xff;
        s += (uint32_t)(x > y ? x - y : y - x);
    }
    return s;
}
static inline uint32_t sxtb16(uint32_t v) {
    return ((uint32_t)(int32_t)(int8_t)v & 0xffffu) | ((uint32_t)(int32_t)(int8_t)(v >> 16) << 16);
}
static inline uint32_t add16(uint32_t a, uint32_t b) { return ((a + b) & 0xffffu) | (((a >> 16) + (b >> 16)) << 16); }
static inline uint32_t sel_ge(Cpu* c, uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) r |= ((c->ge >> i) & 1 ? a : b) & (0xffu << (8 * i));
    return r;
}
static inline uint32_t ssat(Cpu* c, int32_t v, int bits) {
    int32_t hi = (int32_t)((1u << (bits - 1)) - 1), lo = -hi - 1;
    if (v > hi) { c->q = 1; return (uint32_t)hi; }
    if (v < lo) { c->q = 1; return (uint32_t)lo; }
    return (uint32_t)v;
}
static inline uint32_t usat(Cpu* c, int32_t v, int bits) {
    int64_t hi = ((int64_t)1 << bits) - 1;
    if (v < 0) { c->q = 1; return 0; }
    if (v > hi) { c->q = 1; return (uint32_t)hi; }
    return (uint32_t)v;
}
static inline uint32_t sat32(Cpu* c, int64_t v) {
    if (v > 0x7fffffff) { c->q = 1; return 0x7fffffffu; }
    if (v < -0x7fffffffll - 1) { c->q = 1; return 0x80000000u; }
    return (uint32_t)v;
}
static inline uint32_t qadd(Cpu* c, uint32_t m, uint32_t n) { return sat32(c, (int64_t)(int32_t)m + (int32_t)n); }
static inline uint32_t qsub(Cpu* c, uint32_t m, uint32_t n) { return sat32(c, (int64_t)(int32_t)m - (int32_t)n); }
static inline uint32_t qdadd(Cpu* c, uint32_t m, uint32_t n) { return sat32(c, (int64_t)(int32_t)m + (int32_t)sat32(c, 2 * (int64_t)(int32_t)n)); }
static inline uint32_t qdsub(Cpu* c, uint32_t m, uint32_t n) { return sat32(c, (int64_t)(int32_t)m - (int32_t)sat32(c, 2 * (int64_t)(int32_t)n)); }

/* parallel add/subtract: prefix s (GE), q (signed sat), sh (signed halving), u (GE), uq, uh */
#define PAR_LANES(name, W, ST, UT, OPS, RES)                                                                \
    static inline uint32_t name(Cpu* c, uint32_t a, uint32_t b) {                                         \
        uint32_t r = 0, ge = 0; (void)ge; (void)c;                                                        \
        for (int i = 0; i < 32 / W; i++) {                                                                \
            int64_t x = (ST)(UT)(a >> (W * i)), y = (ST)(UT)(b >> (W * i)); int64_t s = OPS;              \
            (void)x; (void)y; RES;                                                                        \
        }                                                                                                 \
        return r;                                                                                         \
    }
#define PUT(v) r |= ((uint32_t)(v) & ((1u << (W * (W == 16) + 8 * (W == 8))) - 1)) << (W * i)
#define GE16(cond) if (cond) ge |= 3u << (2 * i)
#define GE8(cond) if (cond) ge |= 1u << i
static inline int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#undef W
#define W 16
PAR_LANES(par_sadd16, 16, int16_t, uint16_t, x + y, { PUT(s); GE16(s >= 0); c->ge = ge; })
PAR_LANES(par_ssub16, 16, int16_t, uint16_t, x - y, { PUT(s); GE16(s >= 0); c->ge = ge; })
PAR_LANES(par_uadd16, 16, uint16_t, uint16_t, x + y, { PUT(s); GE16(s >= 0x10000); c->ge = ge; })
PAR_LANES(par_usub16, 16, uint16_t, uint16_t, x - y, { PUT(s); GE16(s >= 0); c->ge = ge; })
PAR_LANES(par_qadd16, 16, int16_t, uint16_t, clamp64(x + y, -32768, 32767), { PUT(s); })
PAR_LANES(par_qsub16, 16, int16_t, uint16_t, clamp64(x - y, -32768, 32767), { PUT(s); })
PAR_LANES(par_uqadd16, 16, uint16_t, uint16_t, clamp64(x + y, 0, 65535), { PUT(s); })
PAR_LANES(par_uqsub16, 16, uint16_t, uint16_t, clamp64(x - y, 0, 65535), { PUT(s); })
PAR_LANES(par_shadd16, 16, int16_t, uint16_t, (x + y) >> 1, { PUT(s); })
PAR_LANES(par_shsub16, 16, int16_t, uint16_t, (x - y) >> 1, { PUT(s); })
PAR_LANES(par_uhadd16, 16, uint16_t, uint16_t, (x + y) >> 1, { PUT(s); })
PAR_LANES(par_uhsub16, 16, uint16_t, uint16_t, (x - y) >> 1, { PUT(s); })
#undef W
#define W 8
PAR_LANES(par_sadd8, 8, int8_t, uint8_t, x + y, { PUT(s); GE8(s >= 0); c->ge = ge; })
PAR_LANES(par_ssub8, 8, int8_t, uint8_t, x - y, { PUT(s); GE8(s >= 0); c->ge = ge; })
PAR_LANES(par_uadd8, 8, uint8_t, uint8_t, x + y, { PUT(s); GE8(s >= 0x100); c->ge = ge; })
PAR_LANES(par_usub8, 8, uint8_t, uint8_t, x - y, { PUT(s); GE8(s >= 0); c->ge = ge; })
PAR_LANES(par_qadd8, 8, int8_t, uint8_t, clamp64(x + y, -128, 127), { PUT(s); })
PAR_LANES(par_qsub8, 8, int8_t, uint8_t, clamp64(x - y, -128, 127), { PUT(s); })
PAR_LANES(par_uqadd8, 8, uint8_t, uint8_t, clamp64(x + y, 0, 255), { PUT(s); })
PAR_LANES(par_uqsub8, 8, uint8_t, uint8_t, clamp64(x - y, 0, 255), { PUT(s); })
PAR_LANES(par_shadd8, 8, int8_t, uint8_t, (x + y) >> 1, { PUT(s); })
PAR_LANES(par_shsub8, 8, int8_t, uint8_t, (x - y) >> 1, { PUT(s); })
PAR_LANES(par_uhadd8, 8, uint8_t, uint8_t, (x + y) >> 1, { PUT(s); })
PAR_LANES(par_uhsub8, 8, uint8_t, uint8_t, (x - y) >> 1, { PUT(s); })
#undef W
/* asx/sax: exchange halves of b; lane 0 = a.lo -/+ b.hi, lane 1 = a.hi +/- b.lo */
#define PAR_X(name, ST, UT, LO_OP, HI_OP, CLAMP_LO, CLAMP_HI, GEF, HALVE)                                   \
    static inline uint32_t name(Cpu* c, uint32_t a, uint32_t b) {                                         \
        int64_t al = (ST)(UT)a, ah = (ST)(UT)(a >> 16), bl = (ST)(UT)b, bh = (ST)(UT)(b >> 16);           \
        int64_t lo = al LO_OP bh, hi = ah HI_OP bl;                                                       \
        /* GEF 1: signed, GE when lane >= 0; 2: unsigned, sum lane GE when >= 0x10000, difference when >= 0 */ \
        if (GEF == 1) c->ge = ((lo >= 0) ? 3u : 0u) | ((hi >= 0) ? 0xcu : 0u);                            \
        if (GEF == 2) c->ge = ((#LO_OP[0] == '+' ? lo >= 0x10000 : lo >= 0) ? 3u : 0u) |                  \
                              ((#HI_OP[0] == '+' ? hi >= 0x10000 : hi >= 0) ? 0xcu : 0u);                 \
        if (HALVE) { lo >>= 1; hi >>= 1; }                                                                \
        lo = clamp64(lo, CLAMP_LO, CLAMP_HI); hi = clamp64(hi, CLAMP_LO, CLAMP_HI);                       \
        return ((uint32_t)lo & 0xffffu) | ((uint32_t)hi << 16);                                           \
    }
PAR_X(par_sasx, int16_t, uint16_t, -, +, INT64_MIN, INT64_MAX, 1, 0)
PAR_X(par_ssax, int16_t, uint16_t, +, -, INT64_MIN, INT64_MAX, 1, 0)
PAR_X(par_uasx, uint16_t, uint16_t, -, +, INT64_MIN, INT64_MAX, 2, 0)
PAR_X(par_usax, uint16_t, uint16_t, +, -, INT64_MIN, INT64_MAX, 2, 0)
PAR_X(par_qasx, int16_t, uint16_t, -, +, -32768, 32767, 0, 0)
PAR_X(par_qsax, int16_t, uint16_t, +, -, -32768, 32767, 0, 0)
PAR_X(par_uqasx, uint16_t, uint16_t, -, +, 0, 65535, 0, 0)
PAR_X(par_uqsax, uint16_t, uint16_t, +, -, 0, 65535, 0, 0)
PAR_X(par_shasx, int16_t, uint16_t, -, +, INT64_MIN, INT64_MAX, 0, 1)
PAR_X(par_shsax, int16_t, uint16_t, +, -, INT64_MIN, INT64_MAX, 0, 1)
PAR_X(par_uhasx, uint16_t, uint16_t, -, +, INT64_MIN, INT64_MAX, 0, 1)
PAR_X(par_uhsax, uint16_t, uint16_t, +, -, INT64_MIN, INT64_MAX, 0, 1)

/* CRC32 (ARMv8): reflected polynomials 0xEDB88320 / 0x82F63B78 */
static inline uint32_t crc_bits(uint32_t crc, uint32_t v, int nbits, uint32_t poly) {
    crc ^= v;
    for (int i = 0; i < nbits; i++) crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
    return crc;
}
static inline uint32_t crc32b(uint32_t a, uint32_t b) { return crc_bits(a, b & 0xff, 8, 0xEDB88320u); }
static inline uint32_t crc32h(uint32_t a, uint32_t b) { return crc_bits(a, b & 0xffff, 16, 0xEDB88320u); }
static inline uint32_t crc32w(uint32_t a, uint32_t b) { return crc_bits(a, b, 32, 0xEDB88320u); }
static inline uint32_t crc32cb(uint32_t a, uint32_t b) { return crc_bits(a, b & 0xff, 8, 0x82F63B78u); }
static inline uint32_t crc32ch(uint32_t a, uint32_t b) { return crc_bits(a, b & 0xffff, 16, 0x82F63B78u); }
static inline uint32_t crc32cw(uint32_t a, uint32_t b) { return crc_bits(a, b, 32, 0x82F63B78u); }

static inline uint32_t get_cpsr(Cpu* c) {
    return ((uint32_t)c->n << 31) | ((uint32_t)c->z << 30) | ((uint32_t)c->cf << 29) | ((uint32_t)c->v << 28) |
           ((uint32_t)c->q << 27) | (c->ge << 16) | 0x10u /* user mode, ARM state */;
}
static inline void set_cpsr(Cpu* c, uint32_t v, uint32_t mask) {
    if (mask & 8) { c->n = v >> 31; c->z = (v >> 30) & 1; c->cf = (v >> 29) & 1; c->v = (v >> 28) & 1; c->q = (v >> 27) & 1; }
    if (mask & 4) c->ge = (v >> 16) & 0xf;
}

/* ------------------------------------------------------------------------------------------------- VFP */
static inline uint32_t get_fpscr(Cpu* c) {
    return (c->fpscr & 0x0fffffffu) | ((uint32_t)c->fn << 31) | ((uint32_t)c->fz << 30) | ((uint32_t)c->fc << 29) |
           ((uint32_t)c->fv << 28);
}
static inline void set_fpscr(Cpu* c, uint32_t v) {
    c->fpscr = v & 0x0fffffffu;
    c->fn = v >> 31; c->fz = (v >> 30) & 1; c->fc = (v >> 29) & 1; c->fv = (v >> 28) & 1;
}
/* VCMP result flags: less 1000, equal 0110, greater 0010, unordered 0011 */
static inline void vfp_cmp_d(Cpu* c, double a, double b) {
    if (a != a || b != b) { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 1; }
    else if (a == b) { c->fn = 0; c->fz = 1; c->fc = 1; c->fv = 0; }
    else if (a < b) { c->fn = 1; c->fz = 0; c->fc = 0; c->fv = 0; }
    else { c->fn = 0; c->fz = 0; c->fc = 1; c->fv = 0; }
}
static inline void vfp_cmp_f(Cpu* c, float a, float b) { vfp_cmp_d(c, a, b); }
static inline float round_evenf(float x) { return nearbyintf(x); }  /* host default rounding = nearest-even */
static inline double round_even(double x) { return nearbyint(x); }

/* float -> int: ARM saturates and maps NaN to 0 (x86 would give 0x80000000) */
static inline int32_t cvt_d2s32(double x) {
    if (x != x) return 0;
    if (x >= 2147483647.0) return 2147483647;
    if (x <= -2147483648.0) return (int32_t)0x80000000;
    return (int32_t)x;
}
static inline uint32_t cvt_d2u32(double x) {
    if (x != x || x <= 0) return 0;
    if (x >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)x;
}
static inline int32_t cvt_f2s32(float x) { return cvt_d2s32(x); }
static inline uint32_t cvt_f2u32(float x) { return cvt_d2u32(x); }
/* rounding variants: plain = toward zero; _r = FPSCR mode (assume nearest); _a away; _n even; _p +inf; _m -inf */
static inline double fpscr_round(Cpu* c, double x) {
    switch ((c->fpscr >> 22) & 3) {
        case 1: return ceil(x);
        case 2: return floor(x);
        case 3: return trunc(x);
        default: return nearbyint(x);
    }
}
#define VCVT_FAMILY(K, T, IN)                                                                               \
    static inline T vcvt_##K(Cpu* c, IN x) { (void)c; return cvt_##K(x); }                                \
    static inline T vcvt_##K##_r(Cpu* c, IN x) { return cvt_##K(fpscr_round(c, x)); }                      \
    static inline T vcvt_##K##_a(Cpu* c, IN x) { (void)c; return cvt_##K(round(x)); }                     \
    static inline T vcvt_##K##_n(Cpu* c, IN x) { (void)c; return cvt_##K(nearbyint(x)); }                 \
    static inline T vcvt_##K##_p(Cpu* c, IN x) { (void)c; return cvt_##K(ceil(x)); }                      \
    static inline T vcvt_##K##_m(Cpu* c, IN x) { (void)c; return cvt_##K(floor(x)); }
VCVT_FAMILY(d2s32, int32_t, double)
VCVT_FAMILY(d2u32, uint32_t, double)
VCVT_FAMILY(f2s32, int32_t, float)
VCVT_FAMILY(f2u32, uint32_t, float)

/* ------------------------------------------------------------------------------------------------- NEON */
static inline float fmaxf_arm(float a, float b) { return (a != a || b != b) ? __builtin_nanf("") : (a > b ? a : b); }
static inline float fminf_arm(float a, float b) { return (a != a || b != b) ? __builtin_nanf("") : (a < b ? a : b); }
static inline float recip_est(float x) { return 1.0f / x; }
static inline float rsqrt_est(float x) { return 1.0f / sqrtf(x); }
static inline uint32_t recip_est_u32(uint32_t x) { (void)x; return 0xffffffffu; }

#define SAT_T(T, LO, HI)                                                                                    \
    static inline T sat_add_##T(T a, T b) { return (T)clamp64((int64_t)a + (int64_t)b, LO, HI); }          \
    static inline T sat_sub_##T(T a, T b) { return (T)clamp64((int64_t)a - (int64_t)b, LO, HI); }          \
    static inline T sat_abs_##T(T a) { return (T)clamp64(a < 0 ? -(int64_t)a : (int64_t)a, LO, HI); }      \
    static inline T sat_neg_##T(T a) { return (T)clamp64(-(int64_t)a, LO, HI); }                           \
    static inline T sat_narrow_##T(int64_t v) { return (T)clamp64(v, LO, HI); }
SAT_T(int8_t, -128, 127)
SAT_T(uint8_t, 0, 255)
SAT_T(int16_t, -32768, 32767)
SAT_T(uint16_t, 0, 65535)
SAT_T(int32_t, -2147483648ll, 2147483647ll)
SAT_T(uint32_t, 0, 4294967295ll)
static inline int64_t sat_add_int64_t(int64_t a, int64_t b) { int64_t r; return __builtin_add_overflow(a, b, &r) ? (a < 0 ? INT64_MIN : INT64_MAX) : r; }
static inline int64_t sat_sub_int64_t(int64_t a, int64_t b) { int64_t r; return __builtin_sub_overflow(a, b, &r) ? (a < 0 ? INT64_MIN : INT64_MAX) : r; }
static inline uint64_t sat_add_uint64_t(uint64_t a, uint64_t b) { uint64_t r = a + b; return r < a ? UINT64_MAX : r; }
static inline uint64_t sat_sub_uint64_t(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

/* VSHL/VRSHL by register: signed shift amount from the bottom byte of each lane of b */
#define NEON_SHL_T(T, BITS)                                                                                 \
    static inline T neon_shl_##T(T a, int8_t s8) {                                                        \
        int s = s8; /* int: negating -128 must not wrap */                                                \
        if (s >= 0) return s >= BITS ? (T)0 : (T)(a << s);                                                \
        s = -s;                                                                                           \
        if (s >= BITS) return ((T)-1 < 0 && a < 0) ? (T)-1 : (T)0;                                        \
        return (T)(a >> s);                                                                               \
    }                                                                                                     \
    static inline T neon_rshl_##T(T a, int8_t s8) {                                                       \
        int s = s8;                                                                                       \
        if (s >= 0) return neon_shl_##T(a, s8);                                                           \
        s = -s;                                                                                           \
        if (s > BITS) return 0;                                                                           \
        return (T)(((__int128)a + ((__int128)1 << (s - 1))) >> s);                                        \
    }
NEON_SHL_T(int8_t, 8)
NEON_SHL_T(uint8_t, 8)
NEON_SHL_T(int16_t, 16)
NEON_SHL_T(uint16_t, 16)
NEON_SHL_T(int32_t, 32)
NEON_SHL_T(uint32_t, 32)
NEON_SHL_T(int64_t, 64)
NEON_SHL_T(uint64_t, 64)

static inline void neon_zip(Cpu* c, int d, int m, int nbytes, int esz) {
    uint8_t a[16], b[16], o[32];
    memcpy(a, &c->vr.x[d], nbytes); memcpy(b, &c->vr.x[m], nbytes);
    int n = nbytes / esz;
    for (int i = 0; i < n; i++) { memcpy(o + (2 * i) * esz, a + i * esz, esz); memcpy(o + (2 * i + 1) * esz, b + i * esz, esz); }
    memcpy(&c->vr.x[d], o, nbytes); memcpy(&c->vr.x[m], o + nbytes, nbytes);
}
static inline void neon_uzp(Cpu* c, int d, int m, int nbytes, int esz) {
    uint8_t in[32], o[32];
    memcpy(in, &c->vr.x[d], nbytes); memcpy(in + nbytes, &c->vr.x[m], nbytes);
    int n = nbytes / esz;
    for (int i = 0; i < n; i++) { memcpy(o + i * esz, in + (2 * i) * esz, esz); memcpy(o + nbytes + i * esz, in + (2 * i + 1) * esz, esz); }
    memcpy(&c->vr.x[d], o, nbytes); memcpy(&c->vr.x[m], o + nbytes, nbytes);
}
static inline void neon_trn(Cpu* c, int d, int m, int nbytes, int esz) {
    uint8_t a[16], b[16];
    memcpy(a, &c->vr.x[d], nbytes); memcpy(b, &c->vr.x[m], nbytes);
    int n = nbytes / esz;
    for (int i = 0; i + 1 < n; i += 2) {
        uint8_t t[8];
        memcpy(t, a + (i + 1) * esz, esz); memcpy(a + (i + 1) * esz, b + i * esz, esz); memcpy(b + i * esz, t, esz);
    }
    memcpy(&c->vr.x[d], a, nbytes); memcpy(&c->vr.x[m], b, nbytes);
}
static inline void neon_tbl(Cpu* c, int d, int t0, int nt, int idx, int ext) {
    uint8_t tbl[32], ix[8], out[8];
    for (int i = 0; i < nt; i++) memcpy(tbl + 8 * i, &c->vr.x[(t0 + i) & 31], 8);
    memcpy(ix, &c->vr.x[idx], 8); memcpy(out, &c->vr.x[d], 8);
    for (int i = 0; i < 8; i++) out[i] = ix[i] < 8 * nt ? tbl[ix[i]] : (ext ? out[i] : 0);
    memcpy(&c->vr.x[d], out, 8);
}
/* VLDn/VSTn multiple structures: members m at D(d + m*inc + r), elements interleaved in memory */
static inline void neon_ldst_multi(Cpu* c, uint32_t a, int d, int n, int inc, int regs, int esz, int load) {
    int per = 8 / esz;
    for (int r = 0; r < regs; r++)
        for (int e = 0; e < per; e++)
            for (int m = 0; m < n; m++) {
                uint8_t* reg = (uint8_t*)&c->vr.x[(d + m * inc + r) & 31] + e * esz;
                if (load) memcpy(reg, GPTR(a), esz); else memcpy(GPTR(a), reg, esz);
                a += esz;
            }
}
static inline void neon_ldst_lane(Cpu* c, uint32_t a, int d, int n, int inc, int esz, int idx, int load) {
    for (int m = 0; m < n; m++) {
        uint8_t* reg = (uint8_t*)&c->vr.x[(d + m * inc) & 31] + idx * esz;
        if (load) memcpy(reg, GPTR(a), esz); else memcpy(GPTR(a), reg, esz);
        a += esz;
    }
}
static inline void neon_ld_all(Cpu* c, uint32_t a, int d, int n, int inc, int esz, int regs) {
    for (int m = 0; m < n; m++) {
        uint8_t v[8];
        memcpy(v, GPTR(a), esz);
        a += esz;
        for (int r = 0; r < regs; r++) {
            uint8_t* reg = (uint8_t*)&c->vr.x[(d + m * inc + r) & 31];
            for (int e = 0; e < 8 / esz; e++) memcpy(reg + e * esz, v, esz);
        }
    }
}

#ifdef __cplusplus
}
#endif
