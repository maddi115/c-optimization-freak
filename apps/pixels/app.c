// app.c - the pixels app: a shared 64 x 64 board. Uses ROWS: each square is a row, the engine
// notices which squares changed and sends each browser only those, inside the area it views.
// This is also the template for `make new NAME=myapp`: the smallest complete app.
#include <stdio.h>
#include <string.h>

#include "../../engine/engine.h"
#include "../../engine/hash.h"
#include "gen/protocol.h"

typedef struct {
    uint8_t cells[APP_W * APP_H];   // the rows the engine syncs (1 byte each: a colour)
    uint8_t active[FK_MAX_CLIENTS];
    uint32_t painted;               // total paint strokes, for fun
} State;

static void init(void *v, uint32_t seed) { (void)seed; memset(v, 0, sizeof(State)); }
static void on_load(void *v) { memset(((State *)v)->active, 0, sizeof ((State *)v)->active); }
static void step(void *v) { (void)v; }          // nothing moves by itself
static void server_tick(void *v) { (void)v; }

static int command(void *v, int i, const char *t) {
    State *s = v;
    int cell, color;
    if (!s->active[i] || !app_parse_paint(t, &cell, &color)) return 0;
    if (cell < 0 || cell >= APP_W * APP_H || color < 0 || color >= APP_COLORS) return 0;
    if (s->cells[cell] != color) { s->cells[cell] = (uint8_t)color; s->painted++; }
    return 1;
}

static void on_connect(void *v, int i) { ((State *)v)->active[i] = 1; }
static void on_disconnect(void *v, int i) { ((State *)v)->active[i] = 0; }
static int hello_json(void *v, char *b, size_t n) { (void)v; return app_write_hello(b, n, APP_W, APP_H, APP_COLORS); }
static int client_json(void *v, int i, int force, char *b, size_t n) { (void)v; (void)i; (void)force; (void)b; (void)n; return 0; }

static int status_json(void *v, char *b, size_t n) {
    State *s = v;
    int online = 0;
    for (int i = 0; i < FK_MAX_CLIENTS; i++) online += s->active[i];
    return app_write_board(b, n, (int)s->painted, online);
}

static uint32_t checksum(void *v) { return fk_hash32(((State *)v)->cells, APP_W * APP_H); }

static const char *check(void *v) {
    State *s = v;
    for (int i = 0; i < APP_W * APP_H; i++)
        if (s->cells[i] >= APP_COLORS) return "a square has a colour that does not exist";
    return NULL;
}

static int rows(void *v, uint8_t **ptr, int *row_size) {
    *ptr = ((State *)v)->cells;
    *row_size = 1;
    return APP_W * APP_H;
}

static int random_command(void *v, int i, uint32_t r, char *b, size_t n) {
    (void)v; (void)i;
    if (r % 20 == 0) return snprintf(b, n, "p,%d,%u", -(int)(r % 9), r % 8);   // invalid on purpose
    return snprintf(b, n, "p,%u,%u", (r >> 3) % (APP_W * APP_H), (r >> 16) % APP_COLORS);
}

static void debug_html(void *v, FkOut out) { out("<table><tr><td>paint strokes<td>%u</table>", ((State *)v)->painted); }

AppApi app_api(void) {
    return (AppApi){
        .name = "pixels", .state_size = sizeof(State),
        .init = init, .on_load = on_load, .step = step, .server_tick = server_tick, .command = command,
        .connect = on_connect, .disconnect = on_disconnect, .hello_json = hello_json, .client_json = client_json,
        .status_json = status_json, .checksum = checksum, .check = check,
        .lockstep_bytes = 0,
        .rows = rows,
        .random_command = random_command, .debug_html = debug_html,
    };
}
