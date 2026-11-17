// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.
#pragma once

#define APP_W 64
#define APP_H 64
#define APP_COLORS 8

#ifndef PROTO_CONSTANTS_ONLY
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// browser -> server: "p,cell,color"
static inline int app_parse_paint(const char *s, int *cell, int *color) {
    char extra;
    return sscanf(s, "p,%d,%d%c", cell, color, &extra) == 2; // nothing allowed after the last number
}

// server -> browser: {"t":"hello",...}
static inline int app_write_hello(char *b, size_t n, int w, int h, int colors) {
    return snprintf(b, n, "{\"t\":\"hello\",\"w\":%d,\"h\":%d,\"colors\":%d}", w, h, colors);
}

// server -> browser: {"t":"board",...}
static inline int app_write_board(char *b, size_t n, int painted, int online) {
    return snprintf(b, n, "{\"t\":\"board\",\"painted\":%d,\"online\":%d}", painted, online);
}

#endif
