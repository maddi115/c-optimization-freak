// unity.c - the whole server is ONE compile of this file (faster builds, better optimizing).
// Dev: the app comes from libapp.so (hot reload). Release: it is compiled in, with the page.
#define _GNU_SOURCE
#include "ws.c"
#include "http.c"
#include "sandbox.c"
#ifdef RELEASE
#include APP_FILE       /* e.g. ../apps/ride/app.c */
#include ASSETS_FILE    /* the page, baked in by tools/embed.c */
#endif
#include "main.c"
