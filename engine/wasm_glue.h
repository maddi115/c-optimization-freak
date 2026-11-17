// wasm_glue.h - what every app.wasm exports for tunnel.ts. An app's wasm.c includes its own
// step code, defines the FK_ hooks below (lockstep apps only), then includes this file.
// Apps without wasm.c get engine/client_wasm.c (rows only) automatically.
//
//   #define FK_STATE        pointer to the mirrored state
//   #define FK_STATE_BYTES  how many bytes are mirrored
//   static void fk_app_apply(void *state, int kind, int a, int b, int c);
//   static void fk_app_step(void *state);
//
// Built for the oldest WebAssembly level (-mcpu=mvp) with no C library, so any browser since
// 2017 runs it, on the weakest machine.
#pragma once
#include "client.c"

#define EXPORT(name) __attribute__((export_name(name)))

// The compiler may call these; there is no C library in here, so we provide them.
void *memcpy(void *d, const void *s, unsigned long n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, unsigned long n) {
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, unsigned long n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d; }

#define FK_IN_CAP (256 * 1024)            // biggest message: the engine's send queue is 256 KB
static FkClient fk;
static uint8_t fk_in[FK_IN_CAP];
static uint8_t fk_scratch[32 * FK_CHUNK];  // room to unpack a patch (30 chunks max)

#ifdef FK_STATE_BYTES
static uint32_t fk_sums[FKC_RING * ((FK_STATE_BYTES + FK_CHUNK - 1) / FK_CHUNK)];
#endif

EXPORT("fk_setup") void fk_setup(void) {
#ifdef FK_STATE_BYTES
    fk.state = (uint8_t *)(FK_STATE);
    fk.state_bytes = FK_STATE_BYTES;
    fk.apply = fk_app_apply;
    fk.step = fk_app_step;
    fk.sums = fk_sums;
    fk.nchunks = (FK_STATE_BYTES + FK_CHUNK - 1) / FK_CHUNK;
#endif
    fk.scratch = fk_scratch;
    fk.scratch_cap = (int)sizeof fk_scratch;
    fkc_reset(&fk);
}

EXPORT("fk_reset")       void fk_reset(void)        { fkc_reset(&fk); }
EXPORT("fk_in_ptr")      int fk_in_ptr(void)        { return (int)(long)fk_in; }
EXPORT("fk_in_cap")      int fk_in_cap(void)        { return FK_IN_CAP; }
EXPORT("fk_receive")     int fk_receive(int n)      { return n < 0 || n > FK_IN_CAP ? FKC_BROKEN : fkc_receive(&fk, fk_in, n); }
EXPORT("fk_out_ptr")     int fk_out_ptr(void)       { return (int)(long)fk.out; }
EXPORT("fk_out_len")     int fk_out_len(void)       { return fk.out_len; }
EXPORT("fk_out_clear")   void fk_out_clear(void)    { fk.out_len = 0; }
EXPORT("fk_state_ptr")   int fk_state_ptr(void)     { return (int)(long)fk.state; }
EXPORT("fk_state_bytes") int fk_state_bytes(void)   { return fk.state_bytes; }
EXPORT("fk_synced")      int fk_synced(void)        { return fk.synced; }
EXPORT("fk_tick")        int fk_tick(void)          { return (int)fk.tick; }
EXPORT("fk_matches")     int fk_matches(void)       { return (int)fk.matches; }
EXPORT("fk_heals")       int fk_heals(void)         { return (int)fk.heals; }
EXPORT("fk_fulls")       int fk_fulls(void)         { return (int)fk.fulls; }
EXPORT("fk_rows_ptr")    int fk_rows_ptr(void)      { return (int)(long)fk.rows; }

// ROWS apps: make room for the rows (once), by growing WebAssembly memory.
// (Growing memory replaces memory.buffer, so JavaScript re-creates its views after this.)
EXPORT("fk_rows_alloc") int fk_rows_alloc(int nrows, int row_size) {
    if (fk.rows && fk.nrows == nrows && fk.row_size == row_size) return (int)(long)fk.rows;
    if (nrows <= 0 || row_size <= 0 || nrows > (1 << 26) / row_size) return 0;
    int pages = (nrows * row_size + 65535) / 65536;
    int old = __builtin_wasm_memory_grow(0, pages);
    if (old < 0) return 0;
    fk.rows = (uint8_t *)((long)old * 65536);
    fk.nrows = nrows;
    fk.row_size = row_size;
    return (int)(long)fk.rows;
}
