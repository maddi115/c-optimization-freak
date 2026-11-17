// rides.c - the money and timing math.
#include "rides.h"

#include <math.h>
#include <stdio.h>

#include "gen/protocol.h"

// Speeds: a car drives 1 unit (10 m) per city-second = 36 km/h.
// The sim moves cars 1 unit per tick at 20 ticks a second, so it runs 20x faster than real life.
#define SURGE_RADIUS 150

static float round1(float v) { return roundf(v * 10.0f) / 10.0f; }

// Surge = how many free cars we expected near you vs how many are really there.
// Downtown is crowded with cars -> 1.0x. The quiet edge of the city -> up to 3.0x.
static float surge_at(const City *c, int px, int py) {
    float area_share = 2.0f * SURGE_RADIUS * SURGE_RADIUS / ((float)CITY * CITY); // diamond area / city area
    float expected = c->free_cars * area_share;
    int local = sim_free_within(c, px, py, SURGE_RADIUS);
    float ratio = expected / (local > 0 ? (float)local : 0.5f);
    float s = 1.0f + 0.3f * (ratio - 1.0f);
    return round1(s < 1.0f ? 1.0f : s > 3.0f ? 3.0f : s);
}

void rides_quote(City *c, Ride *r, int px, int py, int dx, int dy) {
    if (r->state == RIDE_ARRIVING || r->state == RIDE_ON_TRIP) return; // already riding
    r->px = sim_snap(px); r->py = sim_snap(py);
    r->dx = sim_snap(dx); r->dy = sim_snap(dy);
    r->n_near = sim_nearest(c, r->px, r->py, 3, r->near, r->near_d);
    int trip = sim_dist(r->px, r->py, r->dx, r->dy);
    r->km = trip / 100.0f;            // 1 unit = 10 m
    r->trip_min = trip / 60.0f;       // 1 unit per second
    r->surge = surge_at(c, r->px, r->py);
    float fare = (2.50f + 1.50f * r->km + 0.25f * r->trip_min) * r->surge;
    r->price = roundf((fare < 7.0f ? 7.0f : fare) * 100.0f) / 100.0f;
    r->car = NO_RIDE;
    r->state = RIDE_QUOTED;
}

int rides_book(City *c, Ride *r, int ride_id) {
    if (r->state != RIDE_QUOTED) return -1;
    int id, d;
    if (sim_nearest(c, r->px, r->py, 1, &id, &d) == 0) return -1; // fresh search: cars moved
    r->car = id;
    sim_cmd_ride(c, id, ride_id);
    sim_cmd_target(c, id, r->px, r->py);
    r->state = RIDE_ARRIVING;
    return 0;
}

void rides_cancel(City *c, Ride *r) {
    if (r->car != NO_RIDE) { sim_cmd_ride(c, r->car, NO_RIDE); r->car = NO_RIDE; } // car roams again
    r->state = RIDE_NONE;
}

static int car_at(const City *c, int car, int x, int y) { return c->x[car] == x && c->y[car] == y; }

int rides_tick(City *c, Ride *r) {
    if (r->state == RIDE_ARRIVING && car_at(c, r->car, r->px, r->py)) {
        sim_cmd_target(c, r->car, r->dx, r->dy);
        r->state = RIDE_ON_TRIP;
        return 1;
    }
    if (r->state == RIDE_ON_TRIP && car_at(c, r->car, r->dx, r->dy)) {
        sim_cmd_ride(c, r->car, NO_RIDE);   // drop off: the car is free again
        r->state = RIDE_DONE;
        return 1;
    }
    return 0;
}

int rides_json(const City *c, const Ride *r, char *buf, size_t n) {
    static const char *names[] = {"none", "quoted", "arriving", "on_trip", "done"};
    int eta = 0; // city seconds left for the current leg
    if (r->state == RIDE_ARRIVING) eta = sim_dist(c->x[r->car], c->y[r->car], r->px, r->py);
    if (r->state == RIDE_ON_TRIP)  eta = sim_dist(c->x[r->car], c->y[r->car], r->dx, r->dy);
    int near[3] = {-1, -1, -1};
    for (int i = 0; i < r->n_near && i < 3; i++) near[i] = r->near[i];
    return app_write_ride(buf, n, names[r->state], r->px, r->py, r->dx, r->dy, near,
                      r->n_near ? r->near_d[0] : 0, r->surge, r->price, r->km, r->trip_min, r->car, eta);
}
