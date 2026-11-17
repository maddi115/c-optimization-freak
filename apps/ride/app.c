// app.c - the ride app, plugged into the engine. Uses LOCKSTEP: browsers run sim.c too.
// Everything the engine does (network, page, replay, chaos, sandbox...) comes for free.
#include "sim.c"
#include "rides.c"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../engine/engine.h"
#include "../../engine/hash.h"
#include "gen/protocol.h"

_Static_assert(APP_EV_TARGET == EV_TARGET && APP_EV_RIDE == EV_RIDE && APP_NO_RIDE == NO_RIDE, "protocol.def != sim.h");

typedef struct {
    City city;                         // FIRST: its first SIM_STATE_BYTES are mirrored in every browser
    Ride rides[FK_MAX_CLIENTS];        // one ride per browser
    uint8_t active[FK_MAX_CLIENTS], dirty[FK_MAX_CLIENTS];
    long ticks;
} RideState;

static void init(void *v, uint32_t seed) {
    RideState *s = v;
    memset(s, 0, sizeof *s);
    sim_init(&s->city, seed);
}

static void on_load(void *v) {           // rides belonged to connections that are gone
    RideState *s = v;
    for (int i = 0; i < NUM_CARS; i++) s->city.ride[i] = NO_RIDE;
    memset(s->rides, 0, sizeof s->rides);
    memset(s->active, 0, sizeof s->active);
    memset(s->dirty, 0, sizeof s->dirty);
    s->city.journal_len = s->city.journal_overflow = 0;
    sim_index(&s->city);
}

static void step(void *v) { sim_step(&((RideState *)v)->city); }

static void server_tick(void *v) {
    RideState *s = v;
    s->ticks++;
    for (int i = 0; i < FK_MAX_CLIENTS; i++)
        if (s->active[i] && rides_tick(&s->city, &s->rides[i])) s->dirty[i] = 1;
    sim_index(&s->city);
}

static int command(void *v, int i, const char *t) {
    RideState *s = v;
    int px, py, dx, dy;
    if (!s->active[i]) return 0;
    if (app_parse_quote(t, &px, &py, &dx, &dy)) {
        if (px < 0 || py < 0 || dx < 0 || dy < 0 || px > CITY || py > CITY || dx > CITY || dy > CITY) return 0;
        rides_quote(&s->city, &s->rides[i], px, py, dx, dy);
    } else if (app_parse_book(t)) rides_book(&s->city, &s->rides[i], i);
    else if (app_parse_cancel(t)) rides_cancel(&s->city, &s->rides[i]);
    else return 0;
    s->dirty[i] = 1;
    return 1;
}

static void on_connect(void *v, int i) {
    RideState *s = v;
    s->active[i] = 1;
    s->rides[i] = (Ride){.state = RIDE_NONE, .car = NO_RIDE};
}

static void on_disconnect(void *v, int i) {
    RideState *s = v;
    rides_cancel(&s->city, &s->rides[i]);
    s->active[i] = 0;
}

static int hello_json(void *v, char *b, size_t n) { (void)v; return app_write_hello(b, n, NUM_CARS, CITY, ROAD); }

static int client_json(void *v, int i, int force, char *b, size_t n) {
    RideState *s = v;
    if (!s->active[i] || s->rides[i].state == RIDE_NONE) { if (!force) return 0; }
    int moving = s->rides[i].state == RIDE_ARRIVING || s->rides[i].state == RIDE_ON_TRIP;
    if (!force && !s->dirty[i] && !(moving && s->ticks % 5 == 0)) return 0;
    s->dirty[i] = 0;
    return rides_json(&s->city, &s->rides[i], b, n);
}

static int status_json(void *v, char *b, size_t n) {
    RideState *s = v;
    return app_write_city(b, n, s->city.free_cars, NUM_CARS);
}

static uint32_t checksum(void *v) { return sim_checksum(&((RideState *)v)->city); }

// The app's own rules. The engine checks them every second live, and every tick in
// --replay and --chaos. Breaking one stops the server loudly (and saves crash.snap).
static const char *check(void *v) {
    RideState *s = v;
    const City *c = &s->city;
    static char why[160];
    int free_count = 0;
    for (int i = 0; i < NUM_CARS; i++) {
        int x = c->x[i], y = c->y[i], at = x % ROAD == 0 && y % ROAD == 0;
        if (x < 0 || y < 0 || x > CITY || y > CITY) { snprintf(why, sizeof why, "car %d is outside the city", i); return why; }
        if (x % ROAD && y % ROAD) { snprintf(why, sizeof why, "car %d is off the roads", i); return why; }
        if (c->left[i] < 0 || c->left[i] > ROAD || (c->left[i] == 0) != at) { snprintf(why, sizeof why, "car %d has a wrong step count", i); return why; }
        if (c->tx[i] % ROAD || c->ty[i] % ROAD || c->tx[i] < 0 || c->ty[i] < 0 || c->tx[i] > CITY || c->ty[i] > CITY) {
            snprintf(why, sizeof why, "car %d drives to a spot that is not a crossing", i); return why;
        }
        if (c->dx[i] * c->dx[i] + c->dy[i] * c->dy[i] > 1) { snprintf(why, sizeof why, "car %d moves diagonally or too fast", i); return why; }
        int r = c->ride[i];
        if (r == NO_RIDE) { free_count++; continue; }
        if (r < 0 || r >= FK_MAX_CLIENTS || !s->active[r] || s->rides[r].car != i) {
            snprintf(why, sizeof why, "car %d is busy with ride %d, which does not own it", i, r); return why;
        }
    }
    for (int r = 0; r < FK_MAX_CLIENTS; r++) {
        if (!s->active[r]) continue;
        int st = s->rides[r].state, car = s->rides[r].car;
        if ((st == RIDE_ARRIVING || st == RIDE_ON_TRIP) && (car < 0 || car >= NUM_CARS || c->ride[car] != r)) {
            snprintf(why, sizeof why, "ride %d is %s but its car does not know", r, st == RIDE_ARRIVING ? "arriving" : "on a trip"); return why;
        }
    }
    if (free_count != c->free_cars) { snprintf(why, sizeof why, "free car count is %d, should be %d", c->free_cars, free_count); return why; }
    return NULL;
}

// ---- lockstep hooks
static void mirror_step(void *m) { sim_step((City *)m); }
static void mirror_apply(void *m, int kind, int a, int b, int c) { sim_apply((City *)m, kind, a, b, c); }
static int take_events(void *v, int16_t (*out)[4], int max, int *overflow) {
    City *c = &((RideState *)v)->city;
    int n = c->journal_len < max ? c->journal_len : max;
    for (int k = 0; k < n; k++) {
        out[k][0] = c->journal[k].kind; out[k][1] = c->journal[k].car;
        out[k][2] = c->journal[k].a;    out[k][3] = c->journal[k].b;
    }
    *overflow = c->journal_overflow || c->journal_len > max;
    c->journal_len = c->journal_overflow = 0;
    return n;
}

// ---- testing and tools
static int random_command(void *v, int i, uint32_t r, char *b, size_t n) {
    (void)v; (void)i;
    switch (r % 10) {
    case 0: case 1: case 2: case 3: case 4:
        return snprintf(b, n, "q,%u,%u,%u,%u", (r >> 4) % 1001, (r >> 9) % 1001, (r >> 14) % 1001, (r >> 19) % 1001);
    case 5: case 6: case 7: return snprintf(b, n, "book");
    case 8: return snprintf(b, n, "cancel");
    default: return snprintf(b, n, "q,%d,5,5,5", -(int)(r % 50));   // invalid on purpose
    }
}

static void debug_html(void *v, FkOut out) {
    RideState *s = v;
    out("<table><tr><td>cars<td>%d<tr><td>free<td>%d<tr><td>on rides<td>%d</table>", NUM_CARS, s->city.free_cars,
        NUM_CARS - s->city.free_cars);
}

static void bench(long n, FkOut out) {
    static City a, b;
    sim_init(&a, 42);
    b = a;
    double t0 = 0, t1 = 0, t2 = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); t0 = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
    for (long i = 0; i < n; i++) sim_step(&a);
    clock_gettime(CLOCK_MONOTONIC, &ts); t1 = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
    for (long i = 0; i < n; i++) sim_step_reference(&b);
    clock_gettime(CLOCK_MONOTONIC, &ts); t2 = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
    out("  car movement: fast (SIMD) %.2f us, simple %.2f us per tick (%.1fx), same result: %s\n",
        (t1 - t0) / n, (t2 - t1) / n, (t2 - t1) / (t1 - t0), sim_checksum(&a) == sim_checksum(&b) ? "yes" : "NO");
}

AppApi app_api(void) {
    return (AppApi){
        .name = "ride", .state_size = sizeof(RideState),
        .init = init, .on_load = on_load, .step = step, .server_tick = server_tick, .command = command,
        .connect = on_connect, .disconnect = on_disconnect, .hello_json = hello_json, .client_json = client_json,
        .status_json = status_json, .checksum = checksum, .check = check,
        .lockstep_bytes = SIM_STATE_BYTES, .mirror_size = sizeof(City),
        .mirror_step = mirror_step, .mirror_apply = mirror_apply, .take_events = take_events,
        .rows = NULL,
        .random_command = random_command, .debug_html = debug_html, .bench = bench,
    };
}
