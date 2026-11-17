// fuzz_http.c - throw random requests at the HTTP and WebSocket-handshake parsing.
// Built with clang's libFuzzer + AddressSanitizer: any out-of-bounds read/write stops it.
#define _GNU_SOURCE
#include <sys/stat.h>
#include "../ws.c"
#include "../http.c"

static void make_site(void) {   // a small fake page, so the "found" paths get tested too
    mkdir("/tmp/fk-fuzz-site", 0755);
    mkdir("/tmp/fk-fuzz-site/assets", 0755);
    FILE *f = fopen("/tmp/fk-fuzz-site/index.html", "w"); if (f) { fputs("<html>hi</html>", f); fclose(f); }
    f = fopen("/tmp/fk-fuzz-site/assets/app.js", "w"); if (f) { fputs("console.log(1)", f); fclose(f); }
    f = fopen("/tmp/fk-fuzz-site/assets/app.js.gz", "w"); if (f) { fputs("gzdata", f); fclose(f); }
    http_load("/tmp/fk-fuzz-site");
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static WsConn c;
    static int ready = 0;
    if (!ready) { make_site(); ready = 1; }
    c.fd = -1; c.open = 1; c.upgraded = c.dead = c.close_after = 0; c.out_len = 0;
    size_t m = n < WS_INBUF - 1 ? n : WS_INBUF - 1;
    memcpy(c.in, d, m);
    c.len = m;
    c.in[m] = 0;
    if (!strstr((char *)c.in, "\r\n\r\n")) return 0;   // same rule as the engine: wait for full headers
    if (ws_is_upgrade(&c)) ws_handshake(&c);
    else http_answer(&c);
    return 0;
}
