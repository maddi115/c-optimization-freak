// fuzz_ws.c - throw random WebSocket frames at the frame parser, and every text message
// at the engine's command parsers.
#define _GNU_SOURCE
#include "../ws.c"
#include "../gen/engine_proto.h"

static void on_text(WsConn *c, char *msg, size_t n) {
    (void)c;
    char t[512];
    if (n >= sizeof t) return;
    memcpy(t, msg, n);
    t[n] = 0;
    int a, b;
    eng_parse_join(t, &a); eng_parse_sync(t); eng_parse_sub(t, &a, &b); eng_parse_fix(t, &a); eng_parse_patch(t, &a, &b);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static WsConn c;
    c.fd = -1; c.open = 1; c.upgraded = 1; c.dead = 0; c.out_len = 0;
    size_t m = n < WS_INBUF ? n : WS_INBUF;
    memcpy(c.in, d, m);
    c.len = m;
    ws_read(&c, on_text);
    if (c.len > WS_INBUF) __builtin_trap();
    return 0;
}
