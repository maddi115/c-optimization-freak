// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.
#pragma once

#define ENG_PORT 8080
#define ENG_TICK_MS 50
#define ENG_CHUNK_BYTES 1024
#define ENG_MSG_SNAPSHOT 2
#define ENG_MSG_TICK 3
#define ENG_MSG_ROWS 4
#define ENG_MSG_CHUNKS 5
#define ENG_MSG_PATCH 6
#define ENG_SNAPSHOT_HEADER 12
#define ENG_TICK_HEADER 8
#define ENG_ROWS_HEADER 8
#define ENG_PATCH_HEADER 12
#define ENG_EVENT_BYTES 8
#define ENG_FLAG_CHECKSUM 1
#define ENG_FLAG_ZIP 2
#define ENG_FLAG_FULL 4

#ifndef PROTO_CONSTANTS_ONLY
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// browser -> server: "join,zip"
static inline int eng_parse_join(const char *s, int *zip) {
    char extra;
    return sscanf(s, "join,%d%c", zip, &extra) == 1; // nothing allowed after the last number
}

// browser -> server: "sync"
static inline int eng_parse_sync(const char *s) { return strcmp(s, "sync") == 0; }

// browser -> server: "sub,first,count"
static inline int eng_parse_sub(const char *s, int *first, int *count) {
    char extra;
    return sscanf(s, "sub,%d,%d%c", first, count, &extra) == 2; // nothing allowed after the last number
}

// browser -> server: "fix,tick"
static inline int eng_parse_fix(const char *s, int *tick) {
    char extra;
    return sscanf(s, "fix,%d%c", tick, &extra) == 1; // nothing allowed after the last number
}

// browser -> server: "patch,first,mask"
static inline int eng_parse_patch(const char *s, int *first, int *mask) {
    char extra;
    return sscanf(s, "patch,%d,%d%c", first, mask, &extra) == 2; // nothing allowed after the last number
}

// server -> browser: {"t":"engine",...}
static inline int eng_write_engine(char *b, size_t n, int lockstepBytes, int rows, int rowSize, int tickMs) {
    return snprintf(b, n, "{\"t\":\"engine\",\"lockstepBytes\":%d,\"rows\":%d,\"rowSize\":%d,\"tickMs\":%d}", lockstepBytes, rows, rowSize, tickMs);
}

// server -> browser: {"t":"stats",...}
static inline int eng_write_stats(char *b, size_t n, double simUs, double bytesPerTick, int snapshotBytes, int online, int resyncs, int heals) {
    return snprintf(b, n, "{\"t\":\"stats\",\"simUs\":%.1f,\"bytesPerTick\":%.1f,\"snapshotBytes\":%d,\"online\":%d,\"resyncs\":%d,\"heals\":%d}", simUs, bytesPerTick, snapshotBytes, online, resyncs, heals);
}

#endif
