// client.c - the browser half of the engine, written once in C.
//
// It is compiled into every app's app.wasm (that is what browsers run), and the server's
// --chaos tester uses this exact same code for its simulated browsers. So what the tests
// prove is literally what runs in people's browsers. No C library: it builds for the oldest
// WebAssembly level, which every browser since 2017 can run.
//
// tunnel.ts only does the parts that need a browser: the WebSocket, timing, and JSON.
#pragma once
#include <stdint.h>

#ifndef PROTO_CONSTANTS_ONLY
#define PROTO_CONSTANTS_ONLY   // only the numbers; the browser has no C library
#include "gen/engine_proto.h"
#undef PROTO_CONSTANTS_ONLY
#else
#include "gen/engine_proto.h"
#endif
#include "hash.h"
#include "pack.h"

#define FKC_RING 8      // remember chunk hashes of the last 8 checksums (same as the server)
#define FKC_OUT 2048

typedef struct {
    // lockstep (state_bytes = 0 means the app has none)
    uint8_t *state;
    int state_bytes;
    void (*apply)(void *state, int kind, int a, int b, int c);
    void (*step)(void *state);
    uint32_t *sums;                 // FKC_RING x nchunks
    long sum_tick[FKC_RING];
    int nchunks;
    // rows
    uint8_t *rows;
    int nrows, row_size;
    // room to unpack a patch (at least 30 chunks)
    uint8_t *scratch;
    int scratch_cap;
    // where we are
    int synced, fresh;              // fresh: the last thing applied was a full copy or a patch
    long tick, fix_sent;
    long matches, heals, fulls, rows_full, rows_changed;
    // commands waiting to go to the server, one per line
    char out[FKC_OUT];
    int out_len;
} FkClient;

enum { FKC_STATE = 1, FKC_ROWS = 2, FKC_SEND = 4, FKC_BROKEN = 8 };

static void fkc_reset(FkClient *c) {
    c->synced = 0; c->fresh = 0; c->tick = -1; c->fix_sent = -1000; c->out_len = 0;
    for (int r = 0; r < FKC_RING; r++) c->sum_tick[r] = -1;
}

static void fkc_send(FkClient *c, const char *cmd) {
    int n = 0;
    while (cmd[n]) n++;
    if (c->out_len + n + 1 > FKC_OUT) return;
    for (int i = 0; i < n; i++) c->out[c->out_len++] = cmd[i];
    c->out[c->out_len++] = '\n';
}

static void fkc_num(char *p, long v) {   // tiny number-to-text, no C library
    char t[24];
    int n = 0, neg = v < 0;
    unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) *p++ = '-';
    while (n) *p++ = t[--n];
    *p = 0;
}

static void fkc_send2(FkClient *c, const char *word, long a, int has_b, long b) {
    char m[64];
    int i = 0;
    while (word[i]) { m[i] = word[i]; i++; }
    m[i++] = ',';
    fkc_num(m + i, a);
    if (has_b) { while (m[i]) i++; m[i++] = ','; fkc_num(m + i, b); }
    fkc_send(c, m);
}

static uint32_t fkc_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int fkc_u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int16_t fkc_i16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static void fkc_copy(uint8_t *d, const uint8_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

// Unpack (or copy) a message body into dst, which must take exactly `raw` bytes.
static int fkc_body(const uint8_t *src, int n, int packed, uint8_t *dst, int raw) {
    if (!packed) { if (n != raw) return -1; fkc_copy(dst, src, n); return raw; }
    return pk_unpack(dst, raw, src, n);
}

static int fkc_lost(FkClient *c) { c->synced = 0; fkc_send(c, "sync"); return FKC_SEND; }

// Handle one binary message from the server. Every length is checked: this is network data.
static int fkc_receive(FkClient *c, const uint8_t *d, int n) {
    if (n < 8) return FKC_BROKEN;
    int type = d[0], flags = d[1], packed = (flags & ENG_FLAG_ZIP) != 0;
    long t = (long)fkc_u32(d + 4);

    if (type == ENG_MSG_SNAPSHOT) {
        if (!c->state_bytes || n < ENG_SNAPSHOT_HEADER) return FKC_BROKEN;
        if ((int)fkc_u32(d + 8) != c->state_bytes) return FKC_BROKEN | fkc_lost(c);  // different app code
        if (fkc_body(d + ENG_SNAPSHOT_HEADER, n - ENG_SNAPSHOT_HEADER, packed, c->state, c->state_bytes) < 0)
            return FKC_BROKEN | fkc_lost(c);
        c->tick = t; c->synced = 1; c->fresh = 1; c->fix_sent = -1000; c->fulls++;
        return FKC_STATE;
    }
    if (type == ENG_MSG_TICK) {
        if (!c->state_bytes) return FKC_BROKEN;
        if (!c->synced) return 0;                           // waiting for a full copy
        if (t != c->tick + 1) return fkc_lost(c);          // missed one: can't stay in step
        int count = fkc_u16(d + 2), has_sum = (flags & ENG_FLAG_CHECKSUM) != 0;
        int at = ENG_TICK_HEADER + (has_sum ? 4 : 0);
        if (at + count * ENG_EVENT_BYTES != n) return FKC_BROKEN | fkc_lost(c);
        for (int k = 0; k < count; k++, at += ENG_EVENT_BYTES)
            c->apply(c->state, fkc_i16(d + at), fkc_i16(d + at + 2), fkc_i16(d + at + 4), fkc_i16(d + at + 6));
        c->step(c->state);
        c->tick = t; c->fresh = 0;
        int out = FKC_STATE;
        if (has_sum) {
            int slot = (int)(t / 20 % FKC_RING);
            uint32_t sum = fk_chunk_hashes(c->state, c->state_bytes, c->sums + slot * c->nchunks);
            c->sum_tick[slot] = t;
            if (sum == fkc_u32(d + ENG_TICK_HEADER)) c->matches++;
            else if (t - c->fix_sent > 60) { fkc_send2(c, "fix", t, 0, 0); c->fix_sent = t; out |= FKC_SEND; } // one at a time
        }
        return out;
    }
    if (type == ENG_MSG_CHUNKS) {                           // the server's chunk hashes for a tick we got wrong
        int count = fkc_u16(d + 2), slot = (int)(t / 20 % FKC_RING);
        if (!c->state_bytes || count != c->nchunks || n != ENG_TICK_HEADER + 4 * count) return FKC_BROKEN;
        if (c->sum_tick[slot] != t) return fkc_lost(c);   // we no longer have our own hashes of that tick
        const uint32_t *mine = c->sums + slot * c->nchunks;
        int out = 0;
        for (int first = 0; first < count; first += 30) {
            int mask = 0;
            for (int b = 0; b < 30 && first + b < count; b++)
                if (fkc_u32(d + ENG_TICK_HEADER + 4 * (first + b)) != mine[first + b]) mask |= 1 << b;
            if (mask) { fkc_send2(c, "patch", first, 1, mask); out = FKC_SEND; }
        }
        return out;
    }
    if (type == ENG_MSG_PATCH) {                            // just the chunks we had wrong, at our current tick
        if (!c->state_bytes || n < ENG_PATCH_HEADER) return FKC_BROKEN;
        if (!c->synced || t != c->tick) return fkc_lost(c);
        int count = fkc_u16(d + 2), raw = (int)fkc_u32(d + 8), hdr = ENG_PATCH_HEADER + 2 * count;
        if (count < 1 || count > 30 || hdr > n || raw > c->scratch_cap) return FKC_BROKEN | fkc_lost(c);
        int need = 0;
        for (int k = 0; k < count; k++) {
            int ch = fkc_u16(d + ENG_PATCH_HEADER + 2 * k);
            if (ch >= c->nchunks) return FKC_BROKEN | fkc_lost(c);
            int len = c->state_bytes - ch * FK_CHUNK;
            need += len < FK_CHUNK ? len : FK_CHUNK;
        }
        if (need != raw || fkc_body(d + hdr, n - hdr, packed, c->scratch, raw) < 0) return FKC_BROKEN | fkc_lost(c);
        int off = 0;
        for (int k = 0; k < count; k++) {
            int ch = fkc_u16(d + ENG_PATCH_HEADER + 2 * k), len = c->state_bytes - ch * FK_CHUNK;
            if (len > FK_CHUNK) len = FK_CHUNK;
            fkc_copy(c->state + ch * FK_CHUNK, c->scratch + off, len);
            off += len;
        }
        c->fresh = 1; c->fix_sent = -1000; c->heals++;
        return FKC_STATE;
    }
    if (type == ENG_MSG_ROWS) {
        int size = fkc_u16(d + 2);
        if (!c->nrows || size != c->row_size) return FKC_BROKEN;
        if (flags & ENG_FLAG_FULL) {
            if (n < ENG_ROWS_HEADER + 12) return FKC_BROKEN;
            uint32_t first = fkc_u32(d + 8), count = fkc_u32(d + 12), raw = fkc_u32(d + 16);
            if (first > (uint32_t)c->nrows || count > (uint32_t)c->nrows - first || raw != count * (uint32_t)size) return FKC_BROKEN;
            if (fkc_body(d + ENG_ROWS_HEADER + 12, n - ENG_ROWS_HEADER - 12, packed, c->rows + first * (uint32_t)size, (int)raw) < 0)
                return FKC_BROKEN | fkc_lost(c);
            c->rows_full++;
        } else {
            if (n < ENG_ROWS_HEADER + 4) return FKC_BROKEN;
            uint32_t count = fkc_u32(d + 8);
            if ((uint64_t)count * (uint64_t)(4 + size) != (uint64_t)(n - ENG_ROWS_HEADER - 4)) return FKC_BROKEN;
            const uint8_t *e = d + ENG_ROWS_HEADER + 4;
            for (uint32_t k = 0; k < count; k++, e += 4 + size) {
                uint32_t r = fkc_u32(e);
                if (r >= (uint32_t)c->nrows) return FKC_BROKEN;
                fkc_copy(c->rows + r * (uint32_t)size, e + 4, size);
            }
            c->rows_changed++;
        }
        return FKC_ROWS;
    }
    return FKC_BROKEN;
}
