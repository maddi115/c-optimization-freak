// http.h - serves the built SolidJS page (ui/dist) from memory.
#pragma once
#include "ws.h"

// Release build: tools/embed.c turns ui/dist into src/generated/assets.c, an array of these.
typedef struct { const char *path; const unsigned char *data; size_t size; } EmbeddedFile;

// Dev: load every file in `dir` (and one folder down, like dist/assets) into memory.
// Release: use the files baked into the program (dir is ignored).
// Returns how many files were loaded, or -1 if dir/index.html is missing.
int http_load(const char *dir);

// Answer one plain HTTP request that is fully in c->in. Queues the reply
// and marks the connection to close once it is sent.
void http_answer(WsConn *c);
