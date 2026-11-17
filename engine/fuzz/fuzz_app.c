// fuzz_app.c - random commands from random clients, ticks in between, and random lockstep
// events into a browser-style mirror. After EVERY step the app's own rules must hold.
// Input lines: first byte = what to do, the rest = the command text.
#include <stdlib.h>
#include <string.h>
#include APP_FILE

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    static AppApi G;
    static void *s, *fresh, *mirror;
    if (!s) {
        G = app_api();
        s = malloc(G.state_size); fresh = malloc(G.state_size);
        G.init(fresh, 7);
        mirror = calloc(1, G.mirror_size > 0 ? (size_t)G.mirror_size : 1);
    }
    memcpy(s, fresh, G.state_size);
    for (int i = 0; i < 4; i++) G.connect(s, i);
    if (G.lockstep_bytes) memcpy(mirror, s, (size_t)G.lockstep_bytes);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && d[j] != '\n') j++;
        if (j > i) {
            uint8_t op = d[i];
            char text[128];
            size_t len = j - i - 1 < sizeof text - 1 ? j - i - 1 : sizeof text - 1;
            memcpy(text, d + i + 1, len);
            text[len] = 0;
            int client = op % 4;
            if (op < 160) G.command(s, client, text);
            else if (op < 200) { G.step(s); G.server_tick(s); }
            else if (op < 210) { G.disconnect(s, client); G.connect(s, client); }
            else if (G.lockstep_bytes && len >= 8) {   // junk events into a mirror: must not crash it
                int16_t e[4];
                memcpy(e, text, 8);
                G.mirror_apply(mirror, e[0], e[1], e[2], e[3]);
                G.mirror_step(mirror);
            }
            const char *bad = G.check(s);
            if (bad) { fprintf(stderr, "rule broken: %s\n", bad); __builtin_trap(); }
            char out[1024];
            for (int c = 0; c < 4; c++) G.client_json(s, c, 1, out, sizeof out);
        }
        i = j + 1;
    }
    for (int c = 0; c < 4; c++) G.disconnect(s, c);
    const char *bad = G.check(s);
    if (bad) { fprintf(stderr, "rule broken after disconnects: %s\n", bad); __builtin_trap(); }
    return 0;
}
