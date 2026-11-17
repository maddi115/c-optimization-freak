// main.c - the engine. The same for every app; an app only fills in engine.h.
//
//   http://localhost:8080/        the app's page
//   http://localhost:8080/debug   live look inside
//   ws://localhost:8080/          the tunnel (engine.def + the app's protocol.def)
//
//   ./server                  live (records inputs to last-run.log, saves state.snap every second)
//   ./server --fresh          live, ignore the saved state
//   ./server --replay         re-run last-run.* offline and compare checksums tick by tick
//   ./server --chaos [N] [T]  N seeds x T ticks of simulated users, lag, drops and corruption
//   ./server --bench [N]      time N ticks
//   ./server --no-sandbox     live without the seccomp/Landlock lockdown
//   ./server --viewer         scrub through last-run.* in the real app page: http://localhost:8080/viewer

#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "gen/engine_proto.h"
#include "hash.h"
#include "http.h"
#include "sandbox.h"
#include "ws.h"

#include "pack.h"     // our own compressor (same code in the browser)
#include "client.c"   // the browser half: --chaos runs it for its simulated browsers

_Static_assert(ENG_CHUNK_BYTES == FK_CHUNK, "engine.def CHUNK_BYTES != hash.h FK_CHUNK");
_Static_assert(ENG_EVENT_BYTES == 4 * sizeof(int16_t), "an event is 4 x int16");

#define PORT ENG_PORT
#define BEHIND (64 * 1024)      // a client with this much unsent data gets a fresh full copy later
#define FULL_GAP 40             // at most one full copy per client every 2 s (except when it joins)
#define SUB_GAP 10          // view changes (sub): at most one fresh copy per 0.5 s
#define JMAX 1024               // most outside events per tick (more = everyone gets a snapshot)
#define RING 8                  // keep chunk hashes of the last 8 checksum ticks (8 s), to answer "fix"
#define PIECE (96 * 1024)       // biggest raw block in one message (fits the 256 KB send queue)
#define SNAPSHOT "state.snap"
#define RECORDING "last-run"
#define LIBAPP "./libapp.so"

// ------------------------------------------------------------------ assertions
// On in production too (TigerBeetle style): stop loudly instead of quietly going wrong.
static AppApi G;
static void *S;                 // the app's state: allocated once, at start
static long ticks;

static void save_state(const char *path);
static void fk_fail(const char *file, int line, const char *what) {
    fprintf(stderr, "\nCHECK FAILED %s:%d: %s (tick %ld)\n", file, line, what, ticks);
    if (S) { save_state("crash.snap"); fprintf(stderr, "state saved to crash.snap\n"); }
    abort();
}
#define CHECK(cond, what) do { if (!(cond)) fk_fail(__FILE__, __LINE__, what); } while (0)

// ------------------------------------------------------------------ state
typedef struct {
    int joined, zip, need_snapshot, need_rows, heal_fails;
    long last_patch, last_full;   // rate limits: a hostile browser can't make us resend 48 KB over and over
    int sub_first, sub_count;
    long rows_sent;
} Peer;

static WsConn conns[FK_MAX_CLIENTS];
static Peer peers[FK_MAX_CLIENTS];
static int active[FK_MAX_CLIENTS];            // connected (live or replayed)

// lockstep
static int LB;                                 // lockstep bytes (0 = off)
static int16_t pre[JMAX][4];
static int npre, pre_overflow, have_sum;
static uint32_t step_sum;
static long ring_tick[RING];
static uint32_t *ring_hash[RING];
static int nchunks;
static uint8_t tickmsg[ENG_TICK_HEADER + 4 + JMAX * ENG_EVENT_BYTES];

// rows
static uint8_t *rows_ptr, *rows_shadow;
static uint32_t *rows_ver;
static int nrows, row_size;

// buffers, allocated once at start
static uint8_t *rawbuf, *msgbuf, *packtmp;
static size_t rawcap, msgcap;

// stats
static double tick_us_sum, tick_us_max, tick_us_last, tick_bytes_sum, start_us;
static long tick_hist[6], resyncs, heals, rec_events, snapshot_tick = -1;
static long why_full[6];   // join, asked (sync), fell behind, fix too old, patches kept failing, event overflow
static int loaded_snapshot;
static FILE *rec;
static uint32_t k_sum;                         // app checksum at this tick (for replay)
static uint8_t *after_step;   // the mirrored bytes right after the last step. Full copies and patches
                              // are sent from THIS, not from the live state: actions done since that
                              // step travel in the next tick message, so a browser must not get them twice.
static int have_k;

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k)); }

// ------------------------------------------------------------------ sending (live or simulated)
static int (*out_bin)(int i, const void *d, size_t n);
static int (*out_txt)(int i, const char *d, size_t n);
static size_t (*out_pending)(int i);

static int live_bin(int i, const void *d, size_t n) { if (ws_send_binary(&conns[i], d, n) < 0) { conns[i].dead = 1; return -1; } return 0; }
static int live_txt(int i, const char *d, size_t n) { if (ws_send_text(&conns[i], d, n) < 0) { conns[i].dead = 1; return -1; } return 0; }
static size_t live_pending(int i) { return conns[i].out_len; }

// Put `raw` after a header in msgbuf, packed (pack.h) if the client accepts it and it helps.
static size_t pack(int i, size_t hdr, const uint8_t *raw, size_t n, uint8_t *flags) {
    CHECK(hdr + (size_t)pk_bound((int)n) <= msgcap, "message buffer too small");
    if (peers[i].zip && n > 64) {
        int z = pk_pack(msgbuf + hdr, packtmp, raw, (int)n);
        if (z > 0 && (size_t)z < n) { *flags |= ENG_FLAG_ZIP; return hdr + (size_t)z; }
    }
    memcpy(msgbuf + hdr, raw, n);
    return hdr + n;
}

// ------------------------------------------------------------------ lockstep messages
static void send_snapshot(int i) {
    uint8_t flags = 0;
    size_t len = pack(i, ENG_SNAPSHOT_HEADER, after_step, (size_t)LB, &flags);
    msgbuf[0] = ENG_MSG_SNAPSHOT; msgbuf[1] = flags; put16(msgbuf + 2, 0);
    put32(msgbuf + 4, (uint32_t)ticks); put32(msgbuf + 8, (uint32_t)LB);
    out_bin(i, msgbuf, len);
    peers[i].need_snapshot = 0;
    peers[i].last_full = ticks;
}

static size_t build_tick(void) {
    uint8_t *p = tickmsg;
    p[0] = ENG_MSG_TICK; p[1] = have_sum ? ENG_FLAG_CHECKSUM : 0;
    put16(p + 2, npre); put32(p + 4, (uint32_t)ticks);
    p += ENG_TICK_HEADER;
    if (have_sum) { put32(p, step_sum); p += 4; }
    for (int k = 0; k < npre; k++, p += ENG_EVENT_BYTES)
        for (int f = 0; f < 4; f++) put16(p + 2 * f, pre[k][f]);
    CHECK((size_t)(p - tickmsg) <= sizeof tickmsg, "tick message overflow");
    return (size_t)(p - tickmsg);
}

// "fix,T": the browser's checksum for tick T was wrong. Send it the chunk hashes of T.
static void send_chunks(int i, long t) {
    for (int r = 0; r < RING; r++) {
        if (ring_tick[r] != t) continue;
        size_t len = ENG_TICK_HEADER + (size_t)nchunks * 4;
        msgbuf[0] = ENG_MSG_CHUNKS; msgbuf[1] = 0; put16(msgbuf + 2, nchunks); put32(msgbuf + 4, (uint32_t)t);
        for (int c = 0; c < nchunks; c++) put32(msgbuf + ENG_TICK_HEADER + 4 * c, ring_hash[r][c]);
        out_bin(i, msgbuf, len);
        return;
    }
    peers[i].need_snapshot = 1; // too old: full copy instead
    why_full[3]++;
}

// "patch,first,mask": send just those chunks of the CURRENT state. The browser gets this
// right after the newest tick message, so it is at exactly the same tick.
static void send_patch(int i, int first, int mask) {
    if (ticks - peers[i].last_patch > 200) peers[i].heal_fails = 0;            // a fresh problem, not the same one
    peers[i].last_patch = ticks;
    if (++peers[i].heal_fails > 8) { peers[i].need_snapshot = 1; why_full[4]++; return; } // keeps failing: full copy
    int idx[32], n = 0;
    for (int b = 0; b < 30; b++)
        if (mask & (1 << b) && first + b >= 0 && first + b < nchunks) idx[n++] = first + b;
    if (!n) return;
    size_t raw = 0;
    for (int k = 0; k < n; k++) {
        int off = idx[k] * FK_CHUNK, len = LB - off < FK_CHUNK ? LB - off : FK_CHUNK;
        memcpy(rawbuf + raw, after_step + off, (size_t)len);
        raw += (size_t)len;
    }
    uint8_t flags = 0;
    size_t hdr = ENG_PATCH_HEADER + 2 * (size_t)n;
    size_t len = pack(i, hdr, rawbuf, raw, &flags);
    msgbuf[0] = ENG_MSG_PATCH; msgbuf[1] = flags; put16(msgbuf + 2, n);
    put32(msgbuf + 4, (uint32_t)ticks); put32(msgbuf + 8, (uint32_t)raw);
    for (int k = 0; k < n; k++) put16(msgbuf + ENG_PATCH_HEADER + 2 * k, idx[k]);
    out_bin(i, msgbuf, len);
    heals++;
}

// ------------------------------------------------------------------ rows messages
// Which rows changed this tick? Compare in blocks of ~256 bytes first (one fast memcmp each);
// only look at single rows inside a block that differs. Most ticks, most blocks are equal.
static void rows_diff(void) {
    int per = row_size >= 256 ? 1 : 256 / row_size;
    for (int r0 = 0; r0 < nrows; r0 += per) {
        int n = nrows - r0 < per ? nrows - r0 : per;
        size_t off = (size_t)r0 * row_size, len = (size_t)n * row_size;
        if (!memcmp(rows_ptr + off, rows_shadow + off, len)) continue;
        for (int r = r0; r < r0 + n; r++) {
            uint8_t *a = rows_ptr + (size_t)r * row_size, *b = rows_shadow + (size_t)r * row_size;
            if (memcmp(a, b, (size_t)row_size)) { memcpy(b, a, (size_t)row_size); rows_ver[r] = (uint32_t)(ticks + 1); } // the tick being finished
        }
    }
}

static void send_rows_full(int i) {
    Peer *p = &peers[i];
    int per = PIECE / row_size;
    if (per < 1) per = 1;
    for (int first = p->sub_first; first < p->sub_first + p->sub_count; first += per) {
        int count = p->sub_first + p->sub_count - first < per ? p->sub_first + p->sub_count - first : per;
        size_t raw = (size_t)count * row_size;
        uint8_t flags = ENG_FLAG_FULL;
        size_t len = pack(i, ENG_ROWS_HEADER + 12, rows_shadow + (size_t)first * row_size, raw, &flags);
        msgbuf[0] = ENG_MSG_ROWS; msgbuf[1] = flags; put16(msgbuf + 2, row_size); put32(msgbuf + 4, (uint32_t)ticks);
        put32(msgbuf + 8, (uint32_t)first); put32(msgbuf + 12, (uint32_t)count); put32(msgbuf + 16, (uint32_t)raw);
        out_bin(i, msgbuf, len);
    }
    p->need_rows = 0;
    p->rows_sent = ticks;
    p->last_full = ticks;
}

static void send_rows_delta(int i) {
    Peer *p = &peers[i];
    uint8_t *q = msgbuf + ENG_ROWS_HEADER + 4;
    uint32_t count = 0;
    for (int r = p->sub_first; r < p->sub_first + p->sub_count; r++) {
        if ((long)rows_ver[r] <= p->rows_sent) continue;
        if ((size_t)(q - msgbuf) + 4 + (size_t)row_size > msgcap || (size_t)(q - msgbuf) > PIECE) { send_rows_full(i); return; }
        put32(q, (uint32_t)r);
        memcpy(q + 4, rows_shadow + (size_t)r * row_size, (size_t)row_size);
        q += 4 + row_size;
        count++;
    }
    p->rows_sent = ticks;
    if (!count) return;           // nothing changed in your range: send nothing at all
    msgbuf[0] = ENG_MSG_ROWS; msgbuf[1] = 0; put16(msgbuf + 2, row_size); put32(msgbuf + 4, (uint32_t)ticks);
    put32(msgbuf + 8, count);
    out_bin(i, msgbuf, (size_t)(q - msgbuf));
}

// ------------------------------------------------------------------ events (live, replay, chaos)
// Read one recording line: "T tick id text...". The text is the rest of the line, spaces included.
static int rec_parse(const char *line, char *type, long *t, long *v, char *text, size_t cap) {
    int off = 0;
    *v = 0; text[0] = 0;
    int got = sscanf(line, "%c %ld %ld%n", type, t, v, &off);
    if (got < 2) return 0;
    if (got == 3 && line[off] == ' ') {
        size_t n = strcspn(line + off + 1, "\r\n");
        if (n >= cap) n = cap - 1;
        memcpy(text, line + off + 1, n);
        text[n] = 0;
    }
    return 1;
}
static void record(char type, int id, const char *text) {
    if (!rec) return;
    if (text) {
        fprintf(rec, "%c %ld %d ", type, ticks, id);
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) if (*p >= 32) fputc(*p, rec);
        fputc('\n', rec);
    }
    else fprintf(rec, "%c %ld %d\n", type, ticks, id);
    rec_events++;
}

static void send_client_json(int i, int force) {
    char m[1024];
    int n = G.client_json(S, i, force, m, sizeof m);
    if (n > 0 && peers[i].joined && n < (int)sizeof m) out_txt(i, m, (size_t)n);
}

static int viewer, playing;          // --viewer: browsers only watch a recording

static void ev_connect(int i) {
    if (viewer) { peers[i] = (Peer){0}; return; }
    active[i] = 1;
    peers[i] = (Peer){0};
    G.connect(S, i);
    record('C', i, NULL);
}

static void ev_disconnect(int i) {
    if (viewer) { peers[i] = (Peer){0}; return; }
    if (!active[i]) return;
    G.disconnect(S, i);
    active[i] = 0;
    peers[i] = (Peer){0};
    record('D', i, NULL);
}

static int ev_command(int i, const char *text) {
    if (!G.command(S, i, text)) return 0;
    record('M', i, text);
    return 1;
}

static void handle_text(int i, const char *text) {
    Peer *p = &peers[i];
    int a, b;
    if (eng_parse_join(text, &a)) {
        p->joined = 1;
        p->zip = a != 0;
        p->last_full = -1000;
        char m[1024];
        int n = eng_write_engine(m, sizeof m, LB, nrows, row_size, ENG_TICK_MS);
        out_txt(i, m, (size_t)n);
        n = G.hello_json(S, m, sizeof m);
        if (n > 0 && n < (int)sizeof m) out_txt(i, m, (size_t)n);
        if (LB) { send_snapshot(i); why_full[0]++; }
        if (nrows) { p->sub_first = 0; p->sub_count = nrows; send_rows_full(i); }
        send_client_json(i, 1);
        return;
    }
    if (!p->joined) return;
    if (eng_parse_sync(text)) { p->need_snapshot = LB != 0; p->need_rows = nrows != 0; resyncs++; why_full[1]++; return; }
    if (eng_parse_sub(text, &a, &b)) {
        if (a < 0) a = 0;
        if (a > nrows) a = nrows;
        if (b < 0) b = 0;
        if (b > nrows - a) b = nrows - a;
        p->sub_first = a; p->sub_count = b; p->need_rows = 1;
        // a person clicked a view button: allow the copy after SUB_GAP instead of FULL_GAP
        if (p->last_full > ticks - FULL_GAP + SUB_GAP) p->last_full = ticks - FULL_GAP + SUB_GAP;
        return;
    }
    if (eng_parse_fix(text, &a)) { if (LB) send_chunks(i, a); return; }
    if (eng_parse_patch(text, &a, &b)) { if (LB) send_patch(i, a, b); return; }
    if (!viewer && ev_command(i, text)) send_client_json(i, 1);
}

// ------------------------------------------------------------------ the tick
static void sim_tick(void) {
    if (LB) {
        npre = G.take_events(S, pre, JMAX, &pre_overflow);
        CHECK(npre >= 0 && npre <= JMAX, "take_events returned a bad count");
    }
    G.step(S);
    if (LB) memcpy(after_step, S, (size_t)LB);           // what a browser has after this tick
    have_sum = LB && (ticks + 1) % 20 == 0;
    if (have_sum) {
        int r = (int)((ticks + 1) / 20 % RING);
        step_sum = fk_chunk_hashes(S, LB, ring_hash[r]);
        ring_tick[r] = ticks + 1;
    }
    have_k = (ticks + 1) % 20 == 0;
    if (have_k) k_sum = G.checksum(S);
    G.server_tick(S);
    if (nrows) rows_diff();
    ticks++;
}

static void serve(int i, size_t tick_len) {
    Peer *p = &peers[i];
    if (!p->joined) return;
    if (LB) {
        if (pre_overflow && !p->need_snapshot) why_full[5]++;
        if (p->need_snapshot || pre_overflow) {
            // includes this tick already; a requested copy waits for the rate limit (an overflow can't wait)
            if (out_pending(i) < BEHIND / 2 && (pre_overflow || ticks - p->last_full >= FULL_GAP)) send_snapshot(i);
            else p->need_snapshot = 1;
        } else if (out_pending(i) > BEHIND) {
            p->need_snapshot = 1;
            why_full[2]++;
        } else if (tick_len) {          // (0 = nothing stepped this time: viewer paused)
            out_bin(i, tickmsg, tick_len);
        }
    }
    if (nrows) {
        if (p->need_rows) { if (out_pending(i) < BEHIND / 2 && ticks - p->last_full >= FULL_GAP) send_rows_full(i); }
        else if (out_pending(i) > BEHIND) p->need_rows = 1;
        else send_rows_delta(i);
    }
    send_client_json(i, 0);
}

static void send_stats(int online) {
    char m[256];
    int n = eng_write_stats(m, sizeof m, tick_us_sum / 20.0, tick_bytes_sum / 20.0,
                            LB ? ENG_SNAPSHOT_HEADER + LB : 0, online, (int)resyncs, (int)heals);
    char a[1024];
    int an = G.status_json(S, a, sizeof a);
    for (int i = 0; i < FK_MAX_CLIENTS; i++) {
        if (!peers[i].joined) continue;
        out_txt(i, m, (size_t)n);
        if (an > 0 && an < (int)sizeof a) out_txt(i, a, (size_t)an);
    }
}

// ------------------------------------------------------------------ snapshot files
typedef struct { char magic[4]; uint32_t size; long ticks; } SnapHeader;

static void save_state(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    SnapHeader h = {{'F', 'K', 'S', '1'}, (uint32_t)G.state_size, ticks};
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(S, G.state_size, 1, f) == 1;
    ok = (fclose(f) == 0) && ok;
    if (ok) rename(tmp, path); else unlink(tmp);
}

static int load_state(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    SnapHeader h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "FKS1", 4) && h.size == G.state_size &&
             fread(S, G.state_size, 1, f) == 1;
    fclose(f);
    return ok;
}

// ------------------------------------------------------------------ hot reload
#ifdef RELEASE
static void load_app(int first) { (void)first; G = app_api(); }
#else
#include <dlfcn.h>
static int reloads;
static void *lib;
static struct timespec lib_time;
static void load_app(int first) {
    struct stat st;
    if (stat(LIBAPP, &st) < 0) { if (first) { fprintf(stderr, "%s missing: run make\n", LIBAPP); exit(1); } return; }
    if (!first && st.st_mtim.tv_sec == lib_time.tv_sec && st.st_mtim.tv_nsec == lib_time.tv_nsec) return;
    static int loads = 0;  // every copy gets a NEW name: dlopen of a loaded name returns the old code
    char copy[64];
    snprintf(copy, sizeof copy, "/tmp/fk-app-%d-%d.so", (int)getpid(), loads++);
    FILE *in = fopen(LIBAPP, "rb"), *out = fopen(copy, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    void *h = dlopen(copy, RTLD_NOW | RTLD_LOCAL);
    unlink(copy);
    lib_time = st.st_mtim;
    if (!h) { fprintf(stderr, "hot reload failed: %s\n", dlerror()); if (first) exit(1); return; }
    AppApi (*get)(void) = (AppApi (*)(void))dlsym(h, "app_api");
    AppApi api = get ? get() : (AppApi){0};
    if (!first) {
        uint8_t *rp; int rs = 0;
        int nr = api.rows ? api.rows(S, &rp, &rs) : 0;
        if (api.state_size != G.state_size || api.lockstep_bytes != G.lockstep_bytes ||
            api.mirror_size != G.mirror_size || nr != nrows || rs != row_size) {
            fprintf(stderr, "hot reload refused: the state changed shape. Run make and restart.\n");
            dlclose(h);
            return;
        }
    }
    if (lib) dlclose(lib);
    lib = h;
    G = api;
    if (!first) { reloads++; printf("hot reload #%d: new %s code is live, state kept\n", reloads, G.name); fflush(stdout); }
}
#endif

// ------------------------------------------------------------------ setup shared by all modes
static void setup(int fresh, uint32_t seed) {
    S = aligned_alloc(64, (G.state_size + 63) / 64 * 64);
    CHECK(S != NULL, "out of memory");
    memset(S, 0, G.state_size);
    if (!fresh && load_state(SNAPSHOT)) { loaded_snapshot = 1; G.on_load(S); }
    else G.init(S, seed);
    LB = G.lockstep_bytes;
    CHECK(LB >= 0 && (size_t)LB <= G.state_size, "lockstep_bytes is bigger than the state");
    CHECK(LB == 0 || (G.mirror_step && G.mirror_apply && G.take_events && G.mirror_size >= LB), "lockstep needs mirror_step/apply/take_events");
    if (LB) { after_step = malloc((size_t)LB); CHECK(after_step, "out of memory"); memcpy(after_step, S, (size_t)LB); }
    nrows = G.rows ? G.rows(S, &rows_ptr, &row_size) : 0;
    CHECK(nrows == 0 || (row_size > 0 && row_size <= PIECE), "bad rows");
    nchunks = LB ? fk_chunks(LB) : 0;
    for (int r = 0; r < RING; r++) { ring_tick[r] = -1; ring_hash[r] = nchunks ? calloc((size_t)nchunks, 4) : NULL; }
    if (nrows) {
        rows_shadow = malloc((size_t)nrows * row_size);
        rows_ver = calloc((size_t)nrows, 4);
        CHECK(rows_shadow && rows_ver, "out of memory");
        memcpy(rows_shadow, rows_ptr, (size_t)nrows * row_size);
    }
    rawcap = (size_t)(LB > PIECE ? LB : PIECE) + FK_CHUNK * 32;
    msgcap = 64 + (size_t)pk_bound((int)rawcap) + (size_t)PIECE;
    rawbuf = malloc(rawcap);
    msgbuf = malloc(msgcap);
    packtmp = malloc((size_t)pk_bound((int)rawcap));
    CHECK(rawbuf && msgbuf && packtmp, "out of memory");
    CHECK((size_t)ENG_SNAPSHOT_HEADER + (size_t)pk_bound(LB) < WS_OUTBUF, "lockstep state too big for one message");
    const char *bad = G.check(S);
    if (bad) fk_fail(__FILE__, __LINE__, bad);
}

// ------------------------------------------------------------------ /debug
static char page[128 * 1024];
static size_t page_len;
static void OUT(const char *fmt, ...) {
    if (page_len >= sizeof page - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(page + page_len, sizeof page - page_len, fmt, ap);
    va_end(ap);
    if (w > 0) page_len += (size_t)w < sizeof page - page_len ? (size_t)w : sizeof page - page_len - 1;
}

static void send_debug(WsConn *c) {
    page_len = 0;
    OUT("<!DOCTYPE html><meta charset=utf-8><meta http-equiv=refresh content=1><title>%s /debug</title>"
        "<style>body{font:14px ui-monospace,monospace;margin:24px;color:#111}td,th{padding:2px 12px 2px 0;text-align:left}"
        "h2{margin:20px 0 6px;font-size:15px}.k{color:#666}</style><h1 style=font-size:20px>%s /debug</h1>", G.name, G.name);
#ifdef RELEASE
    OUT("<div class=k>release build: one static file, page baked in, sandbox %s</div>", fk_sandbox_status());
#else
    OUT("<div class=k>dev build: hot reloads so far: %d, sandbox %s</div>", reloads, fk_sandbox_status());
#endif
    OUT("<h2>Engine</h2><table><tr><td>uptime<td>%.0f s<tr><td>ticks<td>%ld<tr><td>app checksum<td>%u"
        "<tr><td>started from<td>%s<tr><td>last snapshot<td>tick %ld<tr><td>recorded inputs<td>%ld"
        "<tr><td>sync<td>%s%s%s</table>",
        (now_us() - start_us) / 1e6, ticks, G.checksum(S), loaded_snapshot ? SNAPSHOT : "fresh state", snapshot_tick,
        rec_events, LB ? "lockstep " : "", nrows ? "rows " : "", LB || nrows ? "" : "none");
    if (LB) OUT("<h2>Lockstep</h2><table><tr><td>mirrored bytes<td>%d (%d chunks)<tr><td>full re-syncs<td>%ld<tr><td>chunk heals<td>%ld</table>",
                LB, nchunks, resyncs, heals);
    if (nrows) OUT("<h2>Rows</h2><table><tr><td>rows<td>%d x %d bytes</table>", nrows, row_size);
    OUT("<h2>Tick time (budget %d ms)</h2><table><tr><td>last<td>%.1f us<tr><td>max, last 10 s<td>%.1f us</table><table>",
        ENG_TICK_MS, tick_us_last, tick_us_max);
    static const char *labels[] = {"&lt; 25 us", "25-50 us", "50-100 us", "100-250 us", "250 us-1 ms", "&gt; 1 ms"};
    long total = 0;
    for (int i = 0; i < 6; i++) total += tick_hist[i];
    for (int i = 0; i < 6; i++)
        OUT("<tr><td>%s<td>%ld<td><span style='display:inline-block;height:10px;width:%dpx;background:#3b82f6'></span>",
            labels[i], tick_hist[i], total ? (int)(240.0 * tick_hist[i] / total) : 0);
    OUT("</table>");
    if (G.debug_html) { OUT("<h2>App</h2>"); G.debug_html(S, OUT); }
    OUT("<h2>Connections</h2><table><tr><th>slot<th>kind<th>unsent<th>zip<th>needs copy<th>rows range");
    for (int i = 0; i < FK_MAX_CLIENTS; i++) {
        if (!conns[i].open) continue;
        OUT("<tr><td>%d<td>%s<td>%zu<td>%s<td>%s<td>%d+%d", i, conns[i].upgraded ? "tunnel" : "http", conns[i].out_len,
            peers[i].zip ? "yes" : "no", peers[i].need_snapshot || peers[i].need_rows ? "yes" : "no",
            peers[i].sub_first, peers[i].sub_count);
    }
    OUT("</table>");
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", page_len);
    ws_send_raw(c, hdr, (size_t)hl);
    ws_send_raw(c, page, page_len);
    c->close_after = 1;
}


// ------------------------------------------------------------------ viewer
// The whole recording is loaded into memory. Jumping to tick T re-runs it from the start up to T
// (thousands of ticks per millisecond), then every open page gets a fresh copy of that moment.
typedef struct { char type; long t, v; char text[512]; } RecLine;
static RecLine *rec_lines;
static int rec_n, rp_pos;
static long rec_last, rp_bad = -1;
static uint8_t *start_copy;

static int load_recording(void) {
    start_copy = malloc(G.state_size);
    if (!start_copy || !load_state(RECORDING ".start")) { fprintf(stderr, "no usable %s.start\n", RECORDING); return 0; }
    memcpy(start_copy, S, G.state_size);
    FILE *f = fopen(RECORDING ".log", "r");
    if (!f) { fprintf(stderr, "no %s.log\n", RECORDING); return 0; }
    char line[600];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        RecLine r = {0};
        if (line[0] == 'V' || !rec_parse(line, &r.type, &r.t, &r.v, r.text, sizeof r.text)) continue;
        if (r.type == 'E') { rec_last = r.t; break; }
        if (rec_n == cap) { cap = cap ? cap * 2 : 1024; rec_lines = realloc(rec_lines, (size_t)cap * sizeof *rec_lines); CHECK(rec_lines, "out of memory"); }
        rec_lines[rec_n++] = r;
        if (r.t > rec_last) rec_last = r.t;
    }
    fclose(f);
    return 1;
}

static void rp_reset(void) {
    memcpy(S, start_copy, G.state_size);
    G.on_load(S);
    ticks = 0; rp_pos = 0; rp_bad = -1;
    if (LB) memcpy(after_step, S, (size_t)LB);
    if (nrows) { memcpy(rows_shadow, rows_ptr, (size_t)nrows * row_size); memset(rows_ver, 0, (size_t)nrows * 4); }
    for (int r = 0; r < RING; r++) ring_tick[r] = -1;
}

static void rp_apply(const RecLine *r) {
    if (r->type == 'C') G.connect(S, (int)r->v);
    else if (r->type == 'D') G.disconnect(S, (int)r->v);
    else if (r->type == 'M') G.command(S, (int)r->v, r->text);
    else if (r->type == 'K' && (!have_k || (uint32_t)r->v != k_sum) && rp_bad < 0) rp_bad = r->t;
}

static void rp_goto(long target) {
    if (target < 0) target = 0;
    if (target > rec_last) target = rec_last;
    if (target < ticks) rp_reset();
    while (ticks < target) {
        while (rp_pos < rec_n && rec_lines[rp_pos].t <= ticks) rp_apply(&rec_lines[rp_pos++]);
        sim_tick();
    }
    while (rp_pos < rec_n && rec_lines[rp_pos].t == ticks && rec_lines[rp_pos].type == 'K') rp_apply(&rec_lines[rp_pos++]);
}

static void viewer_refresh_all(void) {   // everyone watching gets a copy of this moment
    for (int i = 0; i < FK_MAX_CLIENTS; i++) {
        if (!peers[i].joined) continue;
        peers[i].need_snapshot = LB != 0;
        peers[i].need_rows = nrows != 0;
        peers[i].last_full = -1000;
    }
}

static void json_text(const char *t) {   // a command as a JSON string (commands are short and plain)
    OUT("\"");
    for (; *t; t++) { if (*t == '"' || *t == '\\') OUT("\\%c", *t); else if ((unsigned char)*t >= 32) OUT("%c", *t); }
    OUT("\"");
}

static void send_viewer(WsConn *c) {
    char path[128] = "";
    sscanf((char *)c->in, "GET %127s", path);
    page_len = 0;
    const char *type = "application/json";
    long t;
    if (sscanf(path, "/viewer/go?t=%ld", &t) == 1) { playing = 0; rp_goto(t); viewer_refresh_all(); }
    else if (!strcmp(path, "/viewer/play")) playing = !playing;
    if (!strncmp(path, "/viewer/", 8)) {
        OUT("{\"tick\":%ld,\"last\":%ld,\"bad\":%ld,\"playing\":%d,\"inputs\":[", ticks, rec_last, rp_bad, playing);
        int first = 1, shown = 0;
        for (int k = 0; k < rec_n && shown < 40; k++) {
            RecLine *r = &rec_lines[k];
            if (r->type == 'K' || r->t < ticks - 60 || r->t > ticks + 60) continue;
            OUT("%s[\"%c\",%ld,%ld,", first ? "" : ",", r->type, r->t, r->v);
            json_text(r->text);
            OUT("]");
            first = 0; shown++;
        }
        OUT("]}");
    } else {
        type = "text/html; charset=utf-8";
        OUT("<!DOCTYPE html><meta charset=utf-8><title>%s replay viewer</title><style>"
            "body{font:14px system-ui,sans-serif;margin:0;color:#111}header{padding:12px 16px;border-bottom:1px solid #e5e7eb;background:#f9fafb}"
            "button{padding:4px 10px;margin-right:4px;border:1px solid #d1d5db;border-radius:6px;background:#fff;cursor:pointer}"
            "#s{width:100%%;margin:8px 0}#info{color:#4b5563}#inputs{font:12px ui-monospace,monospace;color:#374151;max-height:72px;overflow:auto;margin-top:6px}"
            "iframe{border:0;width:100%%;height:calc(100vh - 190px)}</style>"
            "<header><b>%s replay viewer</b> <span id=info></span><input id=s type=range min=0 value=0>"
            "<button onclick=go(-200)>&minus;200</button><button onclick=go(-20)>&minus;20</button><button onclick=go(-1)>&minus;1</button>"
            "<button id=p onclick=play()>Play</button><button onclick=go(1)>+1</button><button onclick=go(20)>+20</button><button onclick=go(200)>+200</button>"
            "<div id=inputs></div></header><iframe src=/></iframe><script>"
            "let st={tick:0,last:0};const s=document.getElementById('s');"
            "function show(j){st=j;s.max=j.last;s.value=j.tick;"
            "document.getElementById('info').textContent=' tick '+j.tick+' of '+j.last+' ('+(j.tick/20).toFixed(1)+' s)  '+(j.bad<0?'matches the recording':'differs from the recording from tick '+j.bad);"
            "document.getElementById('p').textContent=j.playing?'Pause':'Play';"
            "document.getElementById('inputs').innerHTML=j.inputs.map(r=>(r[1]==j.tick?'<b>':'')+'tick '+r[1]+'  user '+r[2]+'  '+({C:'joined',D:'left',M:''}[r[0]])+' '+r[3].replace(/</g,'&lt;')+(r[1]==j.tick?'</b>':'')).join('<br>')}"
            "async function to(t){show(await (await fetch('/viewer/go?t='+t)).json())}"
            "function go(d){to(Math.max(0,Math.min(st.last,st.tick+d)))}"
            "async function play(){show(await (await fetch('/viewer/play')).json())}"
            "s.oninput=()=>to(+s.value);setInterval(async()=>{if(st.playing)show(await (await fetch('/viewer/state')).json())},250);to(0);"
            "</script>", G.name, G.name);
    }
    char hdr[256];
    int hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      type, page_len);
    ws_send_raw(c, hdr, (size_t)hl);
    ws_send_raw(c, page, page_len);
    c->close_after = 1;
}

// ------------------------------------------------------------------ live
static int id_of(WsConn *c) { return (int)(c - conns); }

static void on_ws_text(WsConn *c, char *msg, size_t n) {
    char tmp[512];
    if (n >= sizeof tmp) return;
    memcpy(tmp, msg, n);
    tmp[n] = 0;
    handle_text(id_of(c), tmp);
}

static void close_conn(int epfd, WsConn *c) {
    if (c->upgraded) ev_disconnect(id_of(c));
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->open = c->upgraded = c->dead = c->close_after = 0;
    c->len = c->out_len = 0;
}

static void on_readable(int epfd, WsConn *c) {
    for (;;) {
        if (c->len >= WS_INBUF) { close_conn(epfd, c); return; }
        ssize_t r = recv(c->fd, c->in + c->len, WS_INBUF - c->len, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { close_conn(epfd, c); return; }
        if (r > 0) { c->len += (size_t)r; continue; }
        break;
    }
    if (!c->upgraded) {
        if (c->close_after) return;
        c->in[c->len < WS_INBUF ? c->len : WS_INBUF - 1] = 0;
        if (!strstr((char *)c->in, "\r\n\r\n")) return;
        if (!ws_is_upgrade(c)) {
            if (!strncmp((char *)c->in, "GET /debug ", 11)) send_debug(c);
            else if (viewer && !strncmp((char *)c->in, "GET /viewer", 11)) send_viewer(c);
            else http_answer(c);
            return;
        }
        if (ws_handshake(c) < 0) { close_conn(epfd, c); return; }
        ev_connect(id_of(c));   // nothing is sent until the browser says "join"
    }
    if (ws_read(c, on_ws_text) < 0) c->dead = 1;
}

static void reap(int epfd, WsConn *c) {
    if (c->open && (c->dead || (c->close_after && c->out_len == 0))) close_conn(epfd, c);
}

static void viewer_tick(void) {
    size_t len = 0;
    if (playing && ticks < rec_last) { rp_goto(ticks + 1); len = LB ? build_tick() : 0; }
    else playing = 0;
    for (int i = 0; i < FK_MAX_CLIENTS; i++) if (conns[i].upgraded) serve(i, len);
}

static void live_tick(void) {
    if (viewer) { viewer_tick(); return; }
    double t0 = now_us();
    sim_tick();
    size_t len = LB ? build_tick() : 0;
    double us = now_us() - t0;
    tick_us_last = us;
    tick_us_sum += us;
    if (us > tick_us_max) tick_us_max = us;
    tick_hist[us < 25 ? 0 : us < 50 ? 1 : us < 100 ? 2 : us < 250 ? 3 : us < 1000 ? 4 : 5]++;
    tick_bytes_sum += (double)len;
    if (rec && have_k) fprintf(rec, "K %ld %u\n", ticks, k_sum);

    int online = 0;
    for (int i = 0; i < FK_MAX_CLIENTS; i++) {
        if (!conns[i].upgraded) continue;
        online++;
        serve(i, len);
    }
    if (ticks % 20 == 0) {
        send_stats(online);
        tick_us_sum = tick_bytes_sum = 0;
        if (rec) fflush(rec);
        save_state(SNAPSHOT);
        snapshot_tick = ticks;
        const char *bad = G.check(S);           // the app's own rules, checked every second
        if (bad) fk_fail(__FILE__, __LINE__, bad);
        load_app(0);                            // dev: swap in new app code if libapp.so changed
    }
    if (ticks % 200 == 0) tick_us_max = 0;
}

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static int run_live(const char *dist, int fresh, int sandbox) {
    out_bin = live_bin; out_txt = live_txt; out_pending = live_pending;
    int nf = http_load(dist);
    if (nf < 0) { fprintf(stderr, "No %s/index.html found. Build the page first: make page\n", dist); return 1; }
    setup(fresh, (uint32_t)time(NULL));
    if (viewer) {                       // watch the last recording instead of running live
        if (!load_recording()) return 1;
        rp_reset();
    } else {
        save_state(RECORDING ".start");
        rec = fopen(RECORDING ".log", "w");
        if (rec) fprintf(rec, "V 2 %zu\n", G.state_size);
    }

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    listen(lfd, 128);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = {.it_interval = {0, ENG_TICK_MS * 1000000L}, .it_value = {0, ENG_TICK_MS * 1000000L}};
    timerfd_settime(tfd, 0, &its, NULL);
    int epfd = epoll_create1(0);
    ws_set_epoll(epfd);
    static int LISTENER, TIMER;
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = &LISTENER};
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    ev.data.ptr = &TIMER;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    start_us = now_us();

    // Everything is open now: from here on the server can only do what a server must do.
    if (sandbox) fk_sandbox();

    if (viewer)
        printf("%s replay viewer: %ld ticks, %d inputs recorded, sandbox %s\nOpen http://localhost:%d/viewer   (Ctrl+C to stop)\n",
               G.name, rec_last, rec_n, fk_sandbox_status(), PORT);
    else
        printf("%s: %s, %d page files, sandbox %s\nOpen http://localhost:%d   debug: http://localhost:%d/debug   (Ctrl+C to stop)\n",
               G.name, loaded_snapshot ? "restored from " SNAPSHOT : "fresh state", nf, fk_sandbox_status(), PORT, PORT);
    fflush(stdout);

    struct epoll_event events[64];
    while (!stop) {
        int n = epoll_wait(epfd, events, 64, -1);
        for (int e = 0; e < n; e++) {
            void *tag = events[e].data.ptr;
            if (tag == &TIMER) {
                uint64_t fired;
                if (read(tfd, &fired, sizeof fired) == sizeof fired) live_tick();
                for (int i = 0; i < FK_MAX_CLIENTS; i++) reap(epfd, &conns[i]);
            } else if (tag == &LISTENER) {
                int fd;
                while ((fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK)) >= 0) {
                    WsConn *slot = NULL;
                    for (int i = 0; i < FK_MAX_CLIENTS; i++) if (!conns[i].open) { slot = &conns[i]; break; }
                    if (!slot) { close(fd); continue; }
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    *slot = (WsConn){.fd = fd, .open = 1};
                    struct epoll_event cev = {.events = EPOLLIN, .data.ptr = slot};
                    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &cev);
                }
            } else {
                WsConn *c = tag;
                if (events[e].events & EPOLLOUT && ws_flush(c) < 0) c->dead = 1;
                if (events[e].events & (EPOLLIN | EPOLLHUP | EPOLLERR) && c->open && !c->dead) on_readable(epfd, c);
                reap(epfd, c);
            }
        }
    }
    if (viewer) { printf("\nviewer closed\n"); return 0; }
    if (rec) { fprintf(rec, "E %ld\n", ticks); fclose(rec); }
    save_state(SNAPSHOT);
    printf("\nstopped at tick %ld. State saved to %s. Replay this run with: ./server --replay or --viewer\n", ticks, SNAPSHOT);
    return 0;
}

// ------------------------------------------------------------------ replay
static int null_bin(int i, const void *d, size_t n) { (void)i; (void)d; (void)n; return 0; }
static int null_txt(int i, const char *d, size_t n) { (void)i; (void)d; (void)n; return 0; }
static size_t null_pending(int i) { (void)i; return 0; }

static int run_replay(void) {
    out_bin = null_bin; out_txt = null_txt; out_pending = null_pending;
    setup(1, 1);
    if (!load_state(RECORDING ".start")) { fprintf(stderr, "no usable %s.start (did the state change shape?)\n", RECORDING); return 1; }
    G.on_load(S);
    if (nrows) memcpy(rows_shadow, rows_ptr, (size_t)nrows * row_size);
    FILE *f = fopen(RECORDING ".log", "r");
    if (!f) { fprintf(stderr, "no %s.log\n", RECORDING); return 1; }
    char line[600];
    long checks = 0, events = 0, bad_tick = -1;
    double t0 = now_us();
    while (fgets(line, sizeof line, f)) {
        char type, text[512] = "";
        long t, v = 0;
        if (line[0] == 'V') continue;
        if (!rec_parse(line, &type, &t, &v, text, sizeof text)) continue;
        while (ticks < t) {
            sim_tick();
            const char *bad = G.check(S);
            if (bad) fk_fail(__FILE__, __LINE__, bad);
        }
        if (type == 'K') { checks++; if ((!have_k || (uint32_t)v != k_sum) && bad_tick < 0) bad_tick = t; }
        else if (type == 'C') { ev_connect((int)v); events++; }
        else if (type == 'D') { ev_disconnect((int)v); events++; }
        else if (type == 'M') { ev_command((int)v, text); events++; }
        else if (type == 'E') break;
    }
    fclose(f);
    printf("replayed %ld ticks in %.1f ms, %ld inputs, %ld checksums, app rules checked every tick\n",
           ticks, (now_us() - t0) / 1000.0, events, checks);
    if (bad_tick < 0) { printf("MATCH: every checksum is identical to the live run\n"); return 0; }
    printf("DIFFERENT from tick %ld: the code changed since the recording, or something is not deterministic\n", bad_tick);
    return 2;
}

// ------------------------------------------------------------------ chaos
// Simulated browsers on a simulated network, all in this process, hundreds of times faster
// than real time. Each one runs client.c: the SAME code browsers run (compiled to app.wasm).
// Lag spikes, disconnects, junk commands, slow readers and memory corruption in the browser
// copy. After every tick, each settled browser must match the server exactly.
typedef struct VMsg { long at; int text; size_t len; struct VMsg *next; uint8_t data[]; } VMsg;
typedef struct {
    int on, corrupt, delay;
    FkClient cl;                 // the real browser code
    VMsg *head, *tail;
    size_t pending;
    char inbox[8][128];          // commands on their way to the server
    long inbox_at[8];
    int ninbox;
} VClient;

#define VN 6
static VClient vc[VN];
static uint32_t crng = 1;
static long c_full_fixed, c_healed, c_corrupts, c_dropped, c_msgs, c_bytes, c_divergent, c_div_lock, c_div_rows;
static uint32_t chaos_rand(void) { crng ^= crng << 13; crng ^= crng >> 17; crng ^= crng << 5; return crng; }

static int v_push(int i, const void *d, size_t n, int text) {
    VClient *v = &vc[i];
    VMsg *m = malloc(sizeof *m + n);
    CHECK(m != NULL, "out of memory");
    m->at = ticks + v->delay;
    if (v->tail && m->at < v->tail->at) m->at = v->tail->at;   // TCP keeps order
    m->text = text; m->len = n; m->next = NULL;
    memcpy(m->data, d, n);
    if (v->tail) v->tail->next = m; else v->head = m;
    v->tail = m;
    v->pending += n;
    c_msgs++; c_bytes += (long)n;
    return 0;
}
static int v_bin(int i, const void *d, size_t n) { return v_push(i, d, n, 0); }
static int v_txt(int i, const char *d, size_t n) { return v_push(i, d, n, 1); }
static size_t v_pending(int i) { return vc[i].pending; }

static void v_send(int i, const char *cmd) {   // browser -> server, arrives 0-2 ticks later
    VClient *v = &vc[i];
    if (v->ninbox == 8) return;
    snprintf(v->inbox[v->ninbox], sizeof v->inbox[0], "%s", cmd);
    v->inbox_at[v->ninbox++] = ticks + (long)(chaos_rand() % 3);
}

static void v_receive(int i, VMsg *m) {
    VClient *v = &vc[i];
    if (m->text) return;
    int was_corrupt = v->corrupt && m->data[0] == ENG_MSG_SNAPSHOT;
    int r = fkc_receive(&v->cl, m->data, (int)m->len);
    CHECK(!(r & FKC_BROKEN), "the server sent a message the browser code could not read");
    if (was_corrupt) { v->corrupt = 0; c_full_fixed++; }
    if (v->cl.out_len) {       // send the browser's commands, one per line
        char line[128];
        int k = 0;
        for (int j = 0; j < v->cl.out_len; j++) {
            if (v->cl.out[j] == '\n') { line[k] = 0; v_send(i, line); k = 0; }
            else if (k < (int)sizeof line - 1) line[k++] = v->cl.out[j];
        }
        v->cl.out_len = 0;
    }
}

static void v_disconnect(int i) {
    VClient *v = &vc[i];
    if (v->corrupt) c_dropped++;
    while (v->head) { VMsg *n = v->head->next; free(v->head); v->head = n; }
    v->tail = NULL; v->pending = 0; v->ninbox = 0;
    v->on = v->corrupt = 0;
    ev_disconnect(i);
}

static int run_chaos(long seeds, long nticks) {
    out_bin = v_bin; out_txt = v_txt; out_pending = v_pending;
    printf("chaos: %ld seeds x %ld ticks, %d simulated browsers each running client.c (lag, disconnects, junk, slow readers, corruption)\n",
           seeds, nticks, VN);
    double t0 = now_us();
    long fulls = 0, heals = 0, rows_full = 0, rows_changed = 0;
    int first = 1;
    for (long seed = 1; seed <= seeds; seed++) {
        if (first) { setup(1, (uint32_t)seed); first = 0; }
        else {
            G.init(S, (uint32_t)seed);
            if (LB) memcpy(after_step, S, (size_t)LB);
            if (nrows) { memcpy(rows_shadow, rows_ptr, (size_t)nrows * row_size); memset(rows_ver, 0, (size_t)nrows * 4); }
            for (int r = 0; r < RING; r++) ring_tick[r] = -1;
        }
        ticks = 0;
        crng = (uint32_t)seed * 2654435761u + 1;
        for (int i = 0; i < VN; i++) {
            VClient *v = &vc[i];
            if (v->on) v_disconnect(i);
            fulls += v->cl.fulls; heals += v->cl.heals; rows_full += v->cl.rows_full; rows_changed += v->cl.rows_changed;
            FkClient keep = v->cl;
            v->cl = (FkClient){0};
            if (!keep.scratch) {
                keep.state = calloc(1, (size_t)(G.mirror_size > 0 ? G.mirror_size : 1));
                keep.sums = calloc((size_t)FKC_RING * (size_t)(nchunks ? nchunks : 1), 4);
                keep.rows = calloc(1, (size_t)(nrows ? nrows * row_size : 1));
                keep.scratch = malloc(32 * FK_CHUNK);
            }
            v->cl.state = keep.state; v->cl.sums = keep.sums; v->cl.rows = keep.rows; v->cl.scratch = keep.scratch;
            v->cl.scratch_cap = 32 * FK_CHUNK;
            v->cl.state_bytes = LB; v->cl.nchunks = nchunks;
            v->cl.apply = G.mirror_apply; v->cl.step = G.mirror_step;
            v->cl.nrows = nrows; v->cl.row_size = row_size;
        }
        for (long t = 0; t < nticks; t++) {
            for (int i = 0; i < VN; i++) {          // random things happen to each browser
                VClient *v = &vc[i];
                uint32_t r = chaos_rand() % 1000;
                if (!v->on) {
                    if (r < 20) {
                        v->on = 1; v->delay = 0; fkc_reset(&v->cl); ev_connect(i);
                        v_send(i, chaos_rand() & 1 ? "join,1" : "join,0");
                    }
                    continue;
                }
                if (r < 3) { v_disconnect(i); continue; }
                if (r < 40 && G.random_command) {
                    char c[128];
                    if (G.random_command(S, i, chaos_rand(), c, sizeof c) > 0) v_send(i, c);
                } else if (r < 45) {
                    static const char *junk[] = {"", ",", "sync,1", "fix,-5", "patch,0,-1", "patch,99999,1", "sub,-1,-1",
                                                 "sub,999999,5", "join", "q,1,2", "zzzz", "p,999999,3", "book,1"};
                    v_send(i, junk[chaos_rand() % (sizeof junk / sizeof junk[0])]);
                } else if (r < 52) {
                    v->delay = (int)(chaos_rand() % 40);  // lag spike: up to 2 seconds
                } else if (r < 56 && LB && v->cl.synced) {
                    if (!v->corrupt) c_corrupts++;   // (a second hit before healing counts as the same problem)
                    v->cl.state[chaos_rand() % (uint32_t)LB] ^= (uint8_t)(1 + chaos_rand() % 255);   // corrupt the browser's copy
                    v->corrupt = 1;
                } else if (r < 60 && nrows) {
                    int a = (int)(chaos_rand() % (uint32_t)nrows), b = (int)(chaos_rand() % (uint32_t)nrows) + 1;
                    char c[64]; snprintf(c, sizeof c, "sub,%d,%d", a, b); v_send(i, c);
                }
            }
            for (int i = 0; i < VN; i++) {          // commands reach the server
                VClient *v = &vc[i];
                int keep = 0;
                for (int k = 0; k < v->ninbox; k++) {
                    if (v->inbox_at[k] <= ticks && v->on) handle_text(i, v->inbox[k]);
                    else if (v->on) { v->inbox_at[keep] = v->inbox_at[k]; memmove(v->inbox[keep], v->inbox[k], sizeof v->inbox[0]); keep++; }
                }
                v->ninbox = keep;
            }
            sim_tick();
            size_t len = LB ? build_tick() : 0;
            for (int i = 0; i < VN; i++) if (vc[i].on) serve(i, len);
            for (int i = 0; i < VN; i++) {          // messages reach the browsers
                VClient *v = &vc[i];
                while (v->head && v->head->at <= ticks) {
                    VMsg *m = v->head;
                    v->head = m->next;
                    if (!v->head) v->tail = NULL;
                    v->pending -= m->len;
                    v_receive(i, m);
                    free(m);
                }
            }
            const char *bad = G.check(S);
            if (bad) fk_fail(__FILE__, __LINE__, bad);
            for (int i = 0; i < VN; i++) {          // every settled browser must match exactly
                VClient *v = &vc[i];
                if (!v->on || v->head || v->ninbox || !peers[i].joined) continue;
                // (A full copy or patch is the state AFTER the server's own work this tick; the browser
                // re-applies those events next tick, so compare only after a normal tick.)
                if (LB && v->cl.synced && !v->cl.fresh && v->cl.tick == ticks && !peers[i].need_snapshot) {
                    int same = memcmp(v->cl.state, after_step, (size_t)LB) == 0;
                    if (v->corrupt) { if (same) { v->corrupt = 0; c_healed++; } }
                    else if (!same) { c_divergent++; c_div_lock++; v_send(i, "sync"); }
                }
                if (nrows && !peers[i].need_rows && peers[i].rows_sent == ticks) {
                    Peer *p = &peers[i];
                    if (memcmp(v->cl.rows + (size_t)p->sub_first * row_size, rows_shadow + (size_t)p->sub_first * row_size,
                               (size_t)p->sub_count * row_size)) { c_divergent++; c_div_rows++; }
                }
            }
        }
    }
    for (int i = 0; i < VN; i++) { fulls += vc[i].cl.fulls; heals += vc[i].cl.heals; rows_full += vc[i].cl.rows_full; rows_changed += vc[i].cl.rows_changed; }
    double ms = (now_us() - t0) / 1000.0;
    printf("  %ld ticks in %.0f ms (%.0fx faster than real time)\n", seeds * nticks, ms, seeds * nticks * ENG_TICK_MS / (ms ? ms : 1));
    printf("  %ld messages, %ld bytes\n", c_msgs, c_bytes);
    if (LB) {
        long open_now = 0;
        for (int i = 0; i < VN; i++) open_now += vc[i].on && vc[i].corrupt;
        printf("  lockstep: full copies %ld, chunk patches %ld\n", fulls, heals);
        printf("  corrupted browsers %ld: fixed by chunk patch or overwritten %ld, by a full copy %ld, left before fixing %ld, still healing at the very end %ld\n",
               c_corrupts, c_healed, c_full_fixed, c_dropped, open_now);
        printf("  full copies by reason: join %ld, asked %ld, fell behind %ld, fix too old %ld, patches kept failing %ld, event overflow %ld\n",
               why_full[0], why_full[1], why_full[2], why_full[3], why_full[4], why_full[5]);
    }
    if (nrows) printf("  rows: full copies %ld, change messages %ld\n", rows_full, rows_changed);
    printf("  every message from the server was readable by the browser code; app rules held every tick\n");
    printf("  silent divergences (a settled browser not matching the server): %ld (lockstep %ld, rows %ld)\n", c_divergent, c_div_lock, c_div_rows);
    printf(c_divergent ? "FAILED\n" : "PASSED\n");
    return c_divergent ? 2 : 0;
}

// ------------------------------------------------------------------ bench
static void bench_out(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
static int run_bench(long n) {
    out_bin = null_bin; out_txt = null_txt; out_pending = null_pending;
    setup(1, 42);
    double t0 = now_us();
    for (long i = 0; i < n; i++) sim_tick();
    printf("%s: %ld full ticks, %.2f us per tick (step + server work%s%s)\n", G.name, n, (now_us() - t0) / n,
           LB ? " + checksums" : "", nrows ? " + row diff" : "");
    if (G.bench) G.bench(n, bench_out);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    load_app(1);
    const char *dist = "dist";
    int fresh = 0, sandbox = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replay")) return run_replay();
        if (!strcmp(argv[i], "--bench")) return run_bench(i + 1 < argc ? atol(argv[i + 1]) : 2000);
        if (!strcmp(argv[i], "--chaos"))
            return run_chaos(i + 1 < argc ? atol(argv[i + 1]) : 20, i + 2 < argc ? atol(argv[i + 2]) : 2000);
        if (!strcmp(argv[i], "--fresh")) fresh = 1;
        else if (!strcmp(argv[i], "--no-sandbox")) sandbox = 0;
        else if (!strcmp(argv[i], "--viewer")) { viewer = 1; fresh = 1; }
        else if (!strcmp(argv[i], "--dist") && i + 1 < argc) dist = argv[++i];
        else { fprintf(stderr, "usage: %s [--fresh] [--no-sandbox] [--dist DIR] | --viewer | --replay | --chaos [N] [T] | --bench [N]\n", argv[0]); return 1; }
    }
    return run_live(dist, fresh, sandbox);
}
