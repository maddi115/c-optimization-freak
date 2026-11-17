// engine.h - the contract between the engine (never edited per app) and an app.
//
// An app is: one state struct + the functions below + protocol.def + a UI.
// The engine gives it: HTTP + WebSocket on one port, a tick loop, record/replay,
// snapshots to disk, hot reload, /debug, chaos testing, a sandbox, and two ways to keep
// browsers up to date:
//
//   LOCKSTEP   the browser runs the same C step code (compiled to WebAssembly). The engine
//              sends the state once, then only outside events + checksums. Best when most
//              changes are computed (simulations, games, planners).
//
//   ROWS       the app's state includes an array of fixed-size rows. The engine notices
//              which rows changed and sends each browser only the changed rows inside the
//              range it subscribed to. Best when changes come from people or the outside
//              world (boards, chats, dashboards, stores).
//
// An app can use either, both, or neither.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FK_MAX_CLIENTS 256

typedef void (*FkOut)(const char *fmt, ...);   // append text to the /debug page or a report

typedef struct {
    const char *name;
    size_t state_size;   // the engine allocates this once at start. ALL app state lives in it,
                         // so it survives hot reload and can be snapshotted and replayed.

    // ---- required ------------------------------------------------------------------------
    void     (*init)(void *s, uint32_t seed);           // fresh state
    void     (*on_load)(void *s);                       // state came from a snapshot file: clear per-client parts
    void     (*step)(void *s);                          // one tick of deterministic change
    void     (*server_tick)(void *s);                   // server-only work after step (may emit events)
    int      (*command)(void *s, int client, const char *text);  // apply a browser command; 1 = valid
    void     (*connect)(void *s, int client);
    void     (*disconnect)(void *s, int client);
    int      (*hello_json)(void *s, char *b, size_t n);           // sent once after join
    int      (*client_json)(void *s, int client, int force, char *b, size_t n); // >0 = send to this client now
    int      (*status_json)(void *s, char *b, size_t n);          // sent to everyone once a second
    uint32_t (*checksum)(void *s);                      // fingerprint for replay
    const char *(*check)(void *s);                      // invariants: NULL = fine, else what is wrong

    // ---- lockstep (set lockstep_bytes = 0 to skip) --------------------------------------
    int  lockstep_bytes;       // the first N bytes of state are mirrored in the browser
    int  mirror_size;          // bytes a mirror copy needs to run mirror_step (>= lockstep_bytes)
    void (*mirror_step)(void *m);                       // == step, on a mirror (no server-only parts)
    void (*mirror_apply)(void *m, int kind, int a, int b, int c);
    int  (*take_events)(void *s, int16_t (*out)[4], int max, int *overflow);

    // ---- rows (return 0 to skip) --------------------------------------------------------
    int  (*rows)(void *s, uint8_t **ptr, int *row_size); // returns the number of rows

    // ---- testing and tools (optional, may be NULL) --------------------------------------
    int  (*random_command)(void *s, int client, uint32_t r, char *b, size_t n); // for --chaos
    void (*debug_html)(void *s, FkOut out);
    void (*bench)(long n, FkOut out);
} AppApi;

AppApi app_api(void);   // every app defines this one function
