// hash.h - one fast fingerprint, identical on the server and in WebAssembly.
// 8 bytes per step (the old one did 1). Whole-number math only, so every machine agrees.
#pragma once
#include <stdint.h>

#define FK_CHUNK 1024   // the state is checked in 1 KB chunks, so a drift can be fixed chunk by chunk

static inline uint64_t fk_hash64(const void *data, int n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    const uint64_t K = 0x9E3779B97F4A7C15ull;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        __builtin_memcpy(&w, p + i, 8);   // safe unaligned load (one instruction on x86 and wasm)
        h = (h ^ w) * K;
        h ^= h >> 29;
    }
    uint64_t tail = 0;
    for (int k = 0; i < n && k < 8; i++, k++) tail |= (uint64_t)p[i] << (8 * k);   // at most 7 bytes left
    h = (h ^ tail ^ (uint64_t)n) * K;
    return h ^ (h >> 32);
}

static inline uint32_t fk_hash32(const void *data, int n) {
    uint64_t h = fk_hash64(data, n, 0x51ED270B27A3A3A7ull);
    return (uint32_t)(h ^ (h >> 32));
}

static inline int fk_chunks(int bytes) { return (bytes + FK_CHUNK - 1) / FK_CHUNK; }

// Hash every chunk into out[], and return one checksum made from all of them.
static inline uint32_t fk_chunk_hashes(const void *state, int bytes, uint32_t *out) {
    const uint8_t *p = (const uint8_t *)state;
    int n = fk_chunks(bytes);
    for (int i = 0; i < n; i++) {
        int len = bytes - i * FK_CHUNK < FK_CHUNK ? bytes - i * FK_CHUNK : FK_CHUNK;
        out[i] = fk_hash32(p + i * FK_CHUNK, len);
    }
    return fk_hash32(out, n * 4);
}
