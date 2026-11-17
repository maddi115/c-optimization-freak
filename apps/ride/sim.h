// sim.h - the city and the cars. Pure math, no networking.
#pragma once
#include <stdint.h>

#define CITY      1000   // city is 1000 x 1000 units (1 unit = 10 m, so 10 x 10 km)
#define ROAD      50     // a road every 50 units (500 m blocks)
#define CELLS     (CITY / ROAD + 1)  // 21 x 21 grid cells for the fast lookup
#ifndef NUM_CARS
#define NUM_CARS  3000
#endif
#define NO_RIDE   -1
#define JOURNAL_MAX 512

// Changes that come from OUTSIDE the car movement (a ride was booked, a car got a new stop).
// The browser runs the same car code itself, so these are the only things it must be told.
enum { EV_TARGET = 1, EV_RIDE = 2 };
typedef struct { int16_t kind, car, a, b; } SimEvent;  // TARGET: a,b = x,y   RIDE: a = ride id

// Struct of arrays: each field is one tight array, so loops read memory in a straight line.
// Everything the simulation needs lives in here (even the random number state), so:
//   - saving the city is one write of this struct (snapshot)
//   - the same starting City + the same inputs = the exact same run (replay)
//   - code can be hot-reloaded without losing any cars
typedef struct {
    uint32_t rng;                        // random number state
    int16_t x[NUM_CARS], y[NUM_CARS];    // where each car is
    int16_t tx[NUM_CARS], ty[NUM_CARS];  // where it is driving to (always a crossing)
    int16_t dx[NUM_CARS], dy[NUM_CARS];  // direction this block (-1, 0, 1)
    int16_t left[NUM_CARS];              // steps until the next crossing (0 = at one)
    int16_t ride[NUM_CARS];              // NO_RIDE = free car, else the ride it serves

    // Spatial grid, rebuilt every tick: free cars sorted by which 50x50 cell they are in.
    int32_t cell_start[CELLS * CELLS + 1];
    int16_t cell_cars[NUM_CARS];
    int free_cars;

    // Server only: outside changes since the last tick message was sent.
    int journal_len, journal_overflow;
    SimEvent journal[JOURNAL_MAX];
} City;

// Bytes of City that the browser mirrors (and the checksum covers): rng + the car arrays.
#define SIM_STATE_BYTES ((int)__builtin_offsetof(City, cell_start))

void sim_init(City *c, uint32_t seed);
void sim_step(City *c);              // move every car one unit (fast version)
void sim_step_reference(City *c);    // same result, simple version (used to check the fast one)
void sim_index(City *c);             // rebuild the spatial grid
int  sim_snap(int v);                // nearest road line
int  sim_dist(int x1, int y1, int x2, int y2);  // driving distance on a grid (Manhattan)
uint32_t sim_checksum(const City *c);           // fingerprint of the whole city

// Find up to k free cars nearest to (px, py). Returns how many were found.
int  sim_nearest(const City *c, int px, int py, int k, int out_ids[], int out_dist[]);

// Free cars within `radius` driving distance of (px, py).
int  sim_free_within(const City *c, int px, int py, int radius);

// Outside changes. The cmd_ versions also write to the journal (server); apply does not (browser).
void sim_cmd_target(City *c, int car, int x, int y);
void sim_cmd_ride(City *c, int car, int ride);
void sim_apply(City *c, int kind, int car, int a, int b);
