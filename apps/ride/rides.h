// rides.h - quotes, surge pricing and the life of a ride. Uses sim.h, no networking.
#pragma once
#include <stddef.h>
#include "sim.h"

enum { RIDE_NONE, RIDE_QUOTED, RIDE_ARRIVING, RIDE_ON_TRIP, RIDE_DONE };

typedef struct {
    int state;
    int px, py, dx, dy;          // pickup and dropoff (snapped to crossings)
    int near[3], near_d[3], n_near;
    int car;                     // the car doing the ride
    float surge, price, km, trip_min;
} Ride;

void rides_quote(City *c, Ride *r, int px, int py, int dx, int dy);
int  rides_book(City *c, Ride *r, int ride_id);  // 0 = booked, -1 = no car free
void rides_cancel(City *c, Ride *r);
int  rides_tick(City *c, Ride *r);               // 1 = the ride's state changed
int  rides_json(const City *c, const Ride *r, char *buf, size_t n);
