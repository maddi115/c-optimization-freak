// app.c - the todo app on the server. Uses BOTH sync modes:
//   LOCKSTEP for what happens (add / tick / delete / clear): one tiny numbered action each,
//            which every device runs itself with the same todo.c.
//   ROWS     for the text: sent once per item, when it is added. Ticking or deleting an item
//            never resends its text.
#include <stdio.h>
#include <string.h>

#include "../../engine/engine.h"
#include "../../engine/hash.h"
#include "gen/protocol.h"
#include "todo.c"

_Static_assert(APP_MAX == TODO_MAX && APP_TEXT_BYTES == TEXT_BYTES && APP_EV_ADD == EV_ADD && APP_EV_TOGGLE == EV_TOGGLE &&
               APP_EV_DELETE == EV_DELETE && APP_EV_CLEAR == EV_CLEAR, "protocol.def != todo.c");

#define JOURNAL_MAX 128

typedef struct {
    Todos t;                              // FIRST: mirrored on every device
    char text[TODO_MAX][TEXT_BYTES];      // the rows: one line of text per item
    int16_t journal[JOURNAL_MAX][4];      // actions since the last tick (server only)
    int journal_len, journal_overflow;
    uint8_t active[FK_MAX_CLIENTS];
    uint32_t actions;                     // total actions, for /debug
} State;

static void act(State *s, int kind, int a) {     // do it here, and note it for every device
    todo_apply(&s->t, kind, a, 0, 0);
    if (s->journal_len == JOURNAL_MAX) { s->journal_overflow = 1; return; }   // engine sends a full copy
    int16_t *e = s->journal[s->journal_len++];
    e[0] = (int16_t)kind; e[1] = (int16_t)a; e[2] = e[3] = 0;
    s->actions++;
}

static void init(void *v, uint32_t seed) { (void)seed; memset(v, 0, sizeof(State)); }
static void on_load(void *v) {
    State *s = v;
    memset(s->active, 0, sizeof s->active);
    s->journal_len = s->journal_overflow = 0;
}
static void step(void *v) { todo_step(&((State *)v)->t); }
static void server_tick(void *v) { (void)v; }

// "a,<text>": add an item. Text is cleaned: no control characters, at most 63 bytes.
static int add(State *s, const char *text) {
    int id = -1;
    for (int i = 0; i < TODO_MAX && id < 0; i++) if (!s->t.used[i]) id = i;
    if (id < 0) return 0;                                   // list is full
    char clean[TEXT_BYTES] = {0};
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < TEXT_BYTES - 1; p++)
        if (*p >= 32 && *p != 127) clean[n++] = (char)*p;
    while (n && clean[n - 1] == ' ') clean[--n] = 0;
    if (!n) return 0;                                       // empty after cleaning
    memcpy(s->text[id], clean, TEXT_BYTES);                 // the row: sent once
    act(s, EV_ADD, id);                                     // the action: one small number
    return 1;
}

static int command(void *v, int i, const char *t) {
    State *s = v;
    int id;
    if (!s->active[i]) return 0;
    if (!strncmp(t, "a,", 2)) return add(s, t + 2);
    if (app_parse_toggle(t, &id)) { if (id < 0 || id >= TODO_MAX || !s->t.used[id]) return 0; act(s, EV_TOGGLE, id); return 1; }
    if (app_parse_remove(t, &id)) { if (id < 0 || id >= TODO_MAX || !s->t.used[id]) return 0; act(s, EV_DELETE, id); return 1; }
    if (app_parse_clear(t)) { act(s, EV_CLEAR, 0); return 1; }
    return 0;
}

static void on_connect(void *v, int i) { ((State *)v)->active[i] = 1; }
static void on_disconnect(void *v, int i) { ((State *)v)->active[i] = 0; }
static int hello_json(void *v, char *b, size_t n) { (void)v; return app_write_hello(b, n, TODO_MAX, TEXT_BYTES); }
static int client_json(void *v, int i, int force, char *b, size_t n) { (void)v; (void)i; (void)force; (void)b; (void)n; return 0; }

static int status_json(void *v, char *b, size_t n) {
    State *s = v;
    int online = 0, items = 0, left = 0;
    for (int i = 0; i < FK_MAX_CLIENTS; i++) online += s->active[i];
    for (int i = 0; i < TODO_MAX; i++) { items += s->t.used[i]; left += s->t.used[i] && !s->t.done[i]; }
    return app_write_list(b, n, items, left, online);
}

static uint32_t checksum(void *v) {
    State *s = v;
    return fk_hash32(&s->t, sizeof s->t) ^ fk_hash32(s->text, sizeof s->text) * 3u;
}

static const char *check(void *v) {
    State *s = v;
    for (int i = 0; i < TODO_MAX; i++) {
        if (s->t.used[i] > 1 || s->t.done[i] > 1) return "a flag that is not 0 or 1";
        if (s->t.done[i] && !s->t.used[i]) return "an empty slot is ticked";
        if (s->text[i][TEXT_BYTES - 1]) return "a text row has no 0 at the end";
        if (s->t.used[i] && s->t.seq[i] >= s->t.next_seq) return "an item from the future";
    }
    return NULL;
}

static void mirror_step(void *m) { todo_step(m); }
static void mirror_apply(void *m, int kind, int a, int b, int c) { todo_apply(m, kind, a, b, c); }
static int take_events(void *v, int16_t (*out)[4], int max, int *overflow) {
    State *s = v;
    int n = s->journal_len < max ? s->journal_len : max;
    memcpy(out, s->journal, (size_t)n * sizeof s->journal[0]);
    *overflow = s->journal_overflow || s->journal_len > max;
    s->journal_len = s->journal_overflow = 0;
    return n;
}

static int rows(void *v, uint8_t **ptr, int *row_size) {
    *ptr = (uint8_t *)((State *)v)->text;
    *row_size = TEXT_BYTES;
    return TODO_MAX;
}

static int random_command(void *v, int i, uint32_t r, char *b, size_t n) {   // for --chaos
    (void)v; (void)i;
    switch (r % 9) {
    case 0: case 1: case 2: return snprintf(b, n, "a,item %u", r >> 8);
    case 3: case 4: return snprintf(b, n, "t,%u", (r >> 8) % TODO_MAX);
    case 5: return snprintf(b, n, "d,%u", (r >> 8) % TODO_MAX);
    case 6: return snprintf(b, n, "c");
    case 7: return snprintf(b, n, "t,%d", -(int)(r % 50));                    // invalid on purpose
    default: return snprintf(b, n, "a,%c%c", 1 + (int)(r % 20), 1 + (int)(r % 7)); // only control characters: rejected
    }
}

static void debug_html(void *v, FkOut out) {
    State *s = v;
    out("<table><tr><td>actions<td>%u<tr><td>next item number<td>%u</table>", s->actions, s->t.next_seq);
}

AppApi app_api(void) {
    return (AppApi){
        .name = "todo", .state_size = sizeof(State),
        .init = init, .on_load = on_load, .step = step, .server_tick = server_tick, .command = command,
        .connect = on_connect, .disconnect = on_disconnect, .hello_json = hello_json, .client_json = client_json,
        .status_json = status_json, .checksum = checksum, .check = check,
        .lockstep_bytes = sizeof(Todos), .mirror_size = sizeof(Todos),
        .mirror_step = mirror_step, .mirror_apply = mirror_apply, .take_events = take_events,
        .rows = rows,
        .random_command = random_command, .debug_html = debug_html,
    };
}
