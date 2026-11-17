// sim.c - moves the cars on a road grid and answers "who is nearest?" fast.
#include "sim.h"
#include "../../engine/hash.h"

// No C library at all (no math.h, string.h, stdlib.h): the same file also compiles to
// WebAssembly for the browser, and whole-number math gives identical results on every machine.
static int iabs(int v) { return v < 0 ? -v : v; }

static uint32_t next_rand(City *c) { // xorshift32: fast, and its state lives in the City
    uint32_t r = c->rng;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    return c->rng = r;
}


int sim_snap(int v) {
    int s = (v + ROAD / 2) / ROAD * ROAD;
    return s < 0 ? 0 : s > CITY ? CITY : s;
}

int sim_dist(int x1, int y1, int x2, int y2) { return iabs(x1 - x2) + iabs(y1 - y2); }

// Random crossing. Half the trips head downtown (a bell curve around the center),
// so the middle of the city is busy and the edges are quiet. That makes surge pricing real.
// The bell curve is whole numbers only: adding 4 random numbers gives a bell shape
// (the more you add, the rounder it gets), so server and browser always agree exactly.
static int bell(City *c) {
    int sum = 0;
    for (int k = 0; k < 4; k++) sum += (int)(next_rand(c) % 279) - 139;  // spread ~160 units
    return sum;
}

static void random_crossing(City *c, int *x, int *y) {
    if (next_rand(c) & 1) {
        *x = (int)(next_rand(c) % (CITY + 1));
        *y = (int)(next_rand(c) % (CITY + 1));
    } else {
        *x = CITY / 2 + bell(c);
        *y = CITY / 2 + bell(c);
    }
    *x = sim_snap(*x);
    *y = sim_snap(*y);
}

static void send_to(City *c, int car, int x, int y) {
    c->tx[car] = (int16_t)x;
    c->ty[car] = (int16_t)y;
}

void sim_apply(City *c, int kind, int car, int a, int b) {
    if (car < 0 || car >= NUM_CARS) return;
    if (kind == EV_TARGET) send_to(c, car, a, b);
    else if (kind == EV_RIDE) c->ride[car] = (int16_t)a;
}

static void journal(City *c, int kind, int car, int a, int b) {
    if (c->journal_len == JOURNAL_MAX) { c->journal_overflow = 1; return; } // server will resend everything
    c->journal[c->journal_len++] = (SimEvent){(int16_t)kind, (int16_t)car, (int16_t)a, (int16_t)b};
}

void sim_cmd_target(City *c, int car, int x, int y) { sim_apply(c, EV_TARGET, car, x, y); journal(c, EV_TARGET, car, x, y); }
void sim_cmd_ride(City *c, int car, int ride)       { sim_apply(c, EV_RIDE, car, ride, 0); journal(c, EV_RIDE, car, ride, 0); }

// A car standing on a crossing picks its next block (or a new trip if it has arrived).
static void decide(City *c, int i) {
    if (c->x[i] != c->tx[i])      { c->dx[i] = c->x[i] < c->tx[i] ? 1 : -1; c->dy[i] = 0; c->left[i] = ROAD; }
    else if (c->y[i] != c->ty[i]) { c->dx[i] = 0; c->dy[i] = c->y[i] < c->ty[i] ? 1 : -1; c->left[i] = ROAD; }
    else {                        // arrived: stand still this tick
        c->dx[i] = c->dy[i] = 0;
        if (c->ride[i] == NO_RIDE) { // free cars roam to a new spot; ride cars wait for rides.c
            int tx, ty;
            random_crossing(c, &tx, &ty);
            send_to(c, i, tx, ty);
        }
    }
}

// Simple version: one car at a time, checks "am I on a crossing?" with % every tick.
static void step_one(City *c, int i) {
    if (c->x[i] % ROAD == 0 && c->y[i] % ROAD == 0) decide(c, i);
    c->x[i] = (int16_t)(c->x[i] + c->dx[i]);
    c->y[i] = (int16_t)(c->y[i] + c->dy[i]);
    c->left[i] = (int16_t)(c->left[i] - (c->left[i] > 0));
}

void sim_step_reference(City *c) {
    for (int i = 0; i < NUM_CARS; i++) step_one(c, i);
}

// Fast version, in two passes:
//  1. only the few cars standing on a crossing (left == 0) make a decision (branchy, rare)
//  2. every car moves - no ifs at all, so the compiler can move 8-16 cars per instruction (SIMD)
void sim_step(City *c) {
    for (int i = 0; i < NUM_CARS; i++)
        if (c->left[i] == 0) decide(c, i);
    int16_t *restrict x = c->x, *restrict y = c->y, *restrict left = c->left;
    const int16_t *restrict dx = c->dx, *restrict dy = c->dy;
    for (int i = 0; i < NUM_CARS; i++) {
        x[i] = (int16_t)(x[i] + dx[i]);
        y[i] = (int16_t)(y[i] + dy[i]);
        left[i] = (int16_t)(left[i] - (left[i] > 0));
    }
}

void sim_init(City *c, uint32_t seed) {
    __builtin_memset(c, 0, sizeof *c);
    c->rng = seed ? seed : 1;
    for (int i = 0; i < NUM_CARS; i++) {
        int x, y, tx, ty;
        random_crossing(c, &x, &y);
        random_crossing(c, &tx, &ty);
        c->x[i] = (int16_t)x; c->y[i] = (int16_t)y;
        c->tx[i] = (int16_t)tx; c->ty[i] = (int16_t)ty;
        c->ride[i] = NO_RIDE;
        for (int k = (int)(next_rand(c) % ROAD); k > 0; k--) step_one(c, i); // spread cars along the blocks
    }
    sim_index(c);
}

static int cell_of(int x, int y) { return (y / ROAD) * CELLS + (x / ROAD); }

// Counting sort of free cars into grid cells: two passes over the arrays, no malloc.
void sim_index(City *c) {
    int count[CELLS * CELLS] = {0};
    c->free_cars = 0;
    for (int i = 0; i < NUM_CARS; i++)
        if (c->ride[i] == NO_RIDE) { count[cell_of(c->x[i], c->y[i])]++; c->free_cars++; }
    c->cell_start[0] = 0;
    for (int k = 0; k < CELLS * CELLS; k++) c->cell_start[k + 1] = c->cell_start[k] + count[k];
    int fill[CELLS * CELLS];
    __builtin_memcpy(fill, c->cell_start, sizeof fill);
    for (int i = 0; i < NUM_CARS; i++)
        if (c->ride[i] == NO_RIDE) c->cell_cars[fill[cell_of(c->x[i], c->y[i])]++] = (int16_t)i;
}

// Search rings of cells outward from the pickup. Stop once no unchecked ring
// can hold a closer car than the k-th best found so far.
int sim_nearest(const City *c, int px, int py, int k, int out_ids[], int out_dist[]) {
    int found = 0, cx = px / ROAD, cy = py / ROAD;
    for (int r = 0; r < CELLS; r++) {
        if (found == k && (r - 1) * ROAD > out_dist[k - 1]) break;
        for (int gy = cy - r; gy <= cy + r; gy++) {
            for (int gx = cx - r; gx <= cx + r; gx++) {
                if (gx < 0 || gy < 0 || gx >= CELLS || gy >= CELLS) continue;
                if (iabs(gx - cx) != r && iabs(gy - cy) != r) continue; // only the ring's edge
                int cell = gy * CELLS + gx;
                for (int j = c->cell_start[cell]; j < c->cell_start[cell + 1]; j++) {
                    int id = c->cell_cars[j];
                    if (c->ride[id] != NO_RIDE) continue; // booked since the last index
                    int d = sim_dist(px, py, c->x[id], c->y[id]);
                    if (found < k) found++;
                    else if (d >= out_dist[k - 1]) continue;
                    int p = found - 1; // insertion sort into the small result list
                    while (p > 0 && out_dist[p - 1] > d) { out_dist[p] = out_dist[p - 1]; out_ids[p] = out_ids[p - 1]; p--; }
                    out_dist[p] = d; out_ids[p] = id;
                }
            }
        }
    }
    return found;
}

int sim_free_within(const City *c, int px, int py, int radius) {
    int n = 0, cr = radius / ROAD + 1, cx = px / ROAD, cy = py / ROAD;
    for (int gy = cy - cr; gy <= cy + cr; gy++)
        for (int gx = cx - cr; gx <= cx + cr; gx++) {
            if (gx < 0 || gy < 0 || gx >= CELLS || gy >= CELLS) continue;
            int cell = gy * CELLS + gx;
            for (int j = c->cell_start[cell]; j < c->cell_start[cell + 1]; j++) {
                int id = c->cell_cars[j];
                if (sim_dist(px, py, c->x[id], c->y[id]) <= radius) n++;
            }
        }
    return n;
}

// Fingerprint of the mirrored state (same fast 8-byte hash the engine and browser use).
uint32_t sim_checksum(const City *c) { return fk_hash32(c, SIM_STATE_BYTES); }
