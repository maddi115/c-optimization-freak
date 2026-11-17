// pack.h - freak-kit's own compressor. About 100 lines, no library, the same code on the
// server and in the browser's WebAssembly.
//
// The idea: app state is mostly arrays of small numbers. In every block of 64 numbers we store
// the smallest one, the common step between them (if any), and then just enough bits for the
// rest. Directions (-1..1) take 2 bits, step counts (0..50) 6 bits, crossings (0, 50, 100...)
// 5 bits, and a block where every value is the same (like "no ride") takes 2-4 bytes total.
// It tries reading the data as bytes and as 16-bit numbers, and keeps whichever is smaller.
//
// Mostly-empty blocks (a few strokes on a white board) use a SPARSE block instead: a bitmap of
// which elements differ from the smallest, then bits for just those.
//
// Format:  [u32 raw bytes][u8 element size 1|2] then per block of 64 elements:
//          [u8 control: bits 0-4 width, bit 5 has step, bit 6 sparse] [min] ([step])
//          ([bitmap, 1 bit per element] if sparse) [width bits per element (or per set bit)]
//          then, for size 2 with an odd byte count, the last byte as-is.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define PK_BLOCK 64

static inline int pk_bound(int n) { return n + n / 8 + 64; }   // worst case output size (with room to spare)

static inline uint32_t pk_gcd(uint32_t a, uint32_t b) { while (b) { uint32_t t = a % b; a = b; b = t; } return a; }

static inline int pk_get(const uint8_t *src, int size, int i) {   // element i as a signed number
    return size == 1 ? src[i] : (int)(int16_t)(src[2 * i] | src[2 * i + 1] << 8);
}

// Compress with one element size. Returns bytes written to dst.
static inline int pk_pack_as(uint8_t *dst, const uint8_t *src, int n, int size) {
    int count = n / size, o = 0;
    dst[o++] = (uint8_t)n; dst[o++] = (uint8_t)(n >> 8); dst[o++] = (uint8_t)(n >> 16); dst[o++] = (uint8_t)(n >> 24);
    dst[o++] = (uint8_t)size;
    for (int b = 0; b < count; b += PK_BLOCK) {
        int len = count - b < PK_BLOCK ? count - b : PK_BLOCK, lo = pk_get(src, size, b), hi = lo;
        for (int k = 1; k < len; k++) { int v = pk_get(src, size, b + k); if (v < lo) lo = v; if (v > hi) hi = v; }
        uint32_t g = 0;
        for (int k = 0; k < len; k++) g = pk_gcd(g, (uint32_t)(pk_get(src, size, b + k) - lo));
        uint32_t span = g ? (uint32_t)(hi - lo) / g : 0;
        int w = 0;
        while (w < 17 && (span >> w)) w++;
        int step = g > 1, others = 0;
        for (int k = 0; k < len; k++) others += pk_get(src, size, b + k) != lo;
        int sparse = w && (len + 7) / 8 + (others * w + 7) / 8 < (len * w + 7) / 8;
        dst[o++] = (uint8_t)(w | step << 5 | sparse << 6);
        dst[o++] = (uint8_t)lo;
        if (size == 2) dst[o++] = (uint8_t)(lo >> 8);
        if (step) { dst[o++] = (uint8_t)g; if (size == 2) dst[o++] = (uint8_t)(g >> 8); }
        if (sparse) {
            for (int k = 0; k < len; k += 8) {
                uint8_t m = 0;
                for (int j = 0; j < 8 && k + j < len; j++) m |= (uint8_t)((pk_get(src, size, b + k + j) != lo) << j);
                dst[o++] = m;
            }
        }
        uint32_t acc = 0;
        int bits = 0;
        for (int k = 0; k < len && w; k++) {
            int x = pk_get(src, size, b + k);
            if (sparse && x == lo) continue;
            uint32_t v = (uint32_t)(x - lo) / (g ? g : 1);
            acc |= v << bits;
            bits += w;
            while (bits >= 8) { dst[o++] = (uint8_t)acc; acc >>= 8; bits -= 8; }
        }
        if (bits) dst[o++] = (uint8_t)acc;
    }
    if (size == 2 && (n & 1)) dst[o++] = src[n - 1];
    return o;
}

// Compress, trying bytes and 16-bit numbers. dst needs pk_bound(n) bytes; tmp too.
static inline int pk_pack(uint8_t *dst, uint8_t *tmp, const uint8_t *src, int n) {
    int a = pk_pack_as(dst, src, n, 1);
    if (n < 2) return a;
    int b = pk_pack_as(tmp, src, n, 2);
    if (b >= a) return a;
    for (int i = 0; i < b; i++) dst[i] = tmp[i];
    return b;
}

// Decompress into dst (exactly cap bytes expected). Returns bytes written, or -1 if the input
// is broken in any way (every read is checked: this runs on data from the network).
static inline int pk_unpack(uint8_t *dst, int cap, const uint8_t *src, int len) {
    if (len < 5) return -1;
    int n = (int)(src[0] | src[1] << 8 | src[2] << 16 | (uint32_t)src[3] << 24), size = src[4], i = 5;
    if (n != cap || n < 0 || (size != 1 && size != 2)) return -1;
    int count = n / size;
    for (int b = 0; b < count; b += PK_BLOCK) {
        int blen = count - b < PK_BLOCK ? count - b : PK_BLOCK;
        if (i + 1 + size > len) return -1;
        int ctl = src[i++], w = ctl & 31, step = (ctl >> 5) & 1, sparse = (ctl >> 6) & 1;
        if (w > 16 || (ctl & 0x80) || (sparse && !w)) return -1;
        int lo = size == 1 ? src[i] : (int)(int16_t)(src[i] | src[i + 1] << 8);
        i += size;
        uint32_t g = 1;
        if (step) {
            if (i + size > len) return -1;
            g = size == 1 ? src[i] : (uint32_t)(src[i] | src[i + 1] << 8);
            i += size;
        }
        const uint8_t *map = 0;
        if (sparse) { map = src + i; i += (blen + 7) / 8; if (i > len) return -1; }
        uint32_t acc = 0;
        int bits = 0;
        for (int k = 0; k < blen; k++) {
            uint32_t v = 0;
            if (w && (!map || (map[k >> 3] >> (k & 7) & 1))) {
                while (bits < w) { if (i >= len) return -1; acc |= (uint32_t)src[i++] << bits; bits += 8; }
                v = acc & ((1u << w) - 1);
                acc >>= w;
                bits -= w;
            }
            uint32_t x = (uint32_t)lo + v * g;   // unsigned math: broken input can't overflow
            int e = b + k;
            if (size == 1) dst[e] = (uint8_t)x;
            else { dst[2 * e] = (uint8_t)x; dst[2 * e + 1] = (uint8_t)(x >> 8); }
        }
    }
    if (size == 2 && (n & 1)) { if (i >= len) return -1; dst[n - 1] = src[i++]; }
    return i == len ? n : -1;
}
