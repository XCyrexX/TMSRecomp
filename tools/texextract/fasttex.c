// fasttex.c: the slow inner loops of texextract.py (ctypes). Build: clang -O2 -shared -o fasttex.dll fasttex.c
// untile / gob_block_height / tex_hash must stay identical to runtime/src/vk_texture.cpp (the pack is keyed by
// that hash).
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

EXPORT uint32_t gob_block_height(uint32_t rows, uint32_t bh) {
    uint32_t gobs = (rows + 7) / 8;
    while (bh > 1 && gobs <= bh / 2) bh /= 2;
    return bh;
}

EXPORT uint32_t level_size(uint32_t row_bytes, uint32_t rows, uint32_t bh) {
    uint32_t blocks_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    return blocks_x * blocks_y * 512 * bh;
}

EXPORT void untile(const uint8_t* src, uint8_t* dst, uint32_t row_bytes, uint32_t rows, uint32_t bh) {
    uint32_t blocks_x = (row_bytes + 63) / 64, block_bytes = 512 * bh;
    for (uint32_t y = 0; y < rows; y++) {
        uint32_t by = y / (8 * bh), gob_in_block = (y % (8 * bh)) / 8, yy = y % 8;
        for (uint32_t x = 0; x < row_bytes; x += 16) {
            uint32_t bx = x / 64, xx = x % 64;
            uint32_t off = (by * blocks_x + bx) * block_bytes + gob_in_block * 512 + (xx / 32) * 256 + (yy / 2) * 64 +
                           ((xx % 32) / 16) * 32 + (yy % 2) * 16;
            uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
            memcpy(dst + (size_t)y * row_bytes + x, src + off, n);
        }
    }
}

EXPORT uint64_t tex_hash(const uint8_t* p, size_t n) {
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

// CRILAYLA (as tools/cpk.py crilayla): out must hold usize + 0x100 bytes. Returns 0 on bad input.
EXPORT int crilayla(const uint8_t* src, size_t src_len, uint8_t* out, size_t out_len) {
    if (src_len < 16 || memcmp(src, "CRILAYLA", 8)) return 0;
    uint32_t usize, csize;
    memcpy(&usize, src + 8, 4);
    memcpy(&csize, src + 12, 4);
    if (out_len < (size_t)usize + 0x100 || 0x10 + (size_t)csize + 0x100 > src_len) return 0;
    memcpy(out, src + 0x10 + csize, 0x100);
    int64_t inp = 0x10 + (int64_t)csize - 1;
    uint32_t pool = 0, left = 0;
#define BITS(n, v)                                                                    \
    do {                                                                              \
        uint32_t need = (n);                                                          \
        v = 0;                                                                        \
        while (need) {                                                                \
            if (!left) { if (inp < 0) return 0; pool = src[inp--]; left = 8; }        \
            uint32_t take = left < need ? left : need;                                \
            v = (v << take) | ((pool >> (left - take)) & ((1u << take) - 1));         \
            left -= take, need -= take;                                               \
        }                                                                             \
    } while (0)
    int64_t o = (int64_t)usize + 0x100 - 1;
    while (o >= 0x100) {
        uint32_t f;
        BITS(1, f);
        if (f) {
            uint32_t off, n, length = 3;
            BITS(13, off);
            off += 3;
            static const uint32_t widths[3] = {2, 3, 5};
            int k;
            for (k = 0; k < 3; k++) {
                BITS(widths[k], n);
                length += n;
                if (n != (1u << widths[k]) - 1) break;
            }
            if (k == 3)
                do { BITS(8, n); length += n; } while (n == 0xFF);
            for (uint32_t i = 0; i < length && o >= 0; i++, o--) {
                if (o + off >= (int64_t)usize + 0x100) return 0;
                out[o] = out[o + off];
            }
        } else {
            uint32_t v;
            BITS(8, v);
            out[o--] = (uint8_t)v;
        }
    }
    return 1;
}
