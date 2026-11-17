// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.
#pragma once

#define APP_NO_RIDE -1
#define APP_EV_TARGET 1
#define APP_EV_RIDE 2

#ifndef PROTO_CONSTANTS_ONLY
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// browser -> server: "q,px,py,dx,dy"
static inline int app_parse_quote(const char *s, int *px, int *py, int *dx, int *dy) {
    char extra;
    return sscanf(s, "q,%d,%d,%d,%d%c", px, py, dx, dy, &extra) == 4; // nothing allowed after the last number
}

// browser -> server: "book"
static inline int app_parse_book(const char *s) { return strcmp(s, "book") == 0; }

// browser -> server: "cancel"
static inline int app_parse_cancel(const char *s) { return strcmp(s, "cancel") == 0; }

// server -> browser: {"t":"hello",...}
static inline int app_write_hello(char *b, size_t n, int cars, int city, int road) {
    return snprintf(b, n, "{\"t\":\"hello\",\"cars\":%d,\"city\":%d,\"road\":%d}", cars, city, road);
}

// server -> browser: {"t":"ride",...}
static inline int app_write_ride(char *b, size_t n, const char *state, int px, int py, int dx, int dy, const int near[3], int nearEta, double surge, double price, double km, double tripMin, int car, int eta) {
    return snprintf(b, n, "{\"t\":\"ride\",\"state\":\"%s\",\"px\":%d,\"py\":%d,\"dx\":%d,\"dy\":%d,\"near\":[%d,%d,%d],\"nearEta\":%d,\"surge\":%.1f,\"price\":%.2f,\"km\":%.2f,\"tripMin\":%.1f,\"car\":%d,\"eta\":%d}", state, px, py, dx, dy, near[0], near[1], near[2], nearEta, surge, price, km, tripMin, car, eta);
}

// server -> browser: {"t":"city",...}
static inline int app_write_city(char *b, size_t n, int free, int cars) {
    return snprintf(b, n, "{\"t\":\"city\",\"free\":%d,\"cars\":%d}", free, cars);
}

#endif
