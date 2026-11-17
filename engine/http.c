// http.c - hands out the SolidJS page. Files are read once at start-up and kept
// in memory, so a request never touches the disk. Only files in the list can be
// served, so paths like /../../etc/passwd simply are not found.
#define _GNU_SOURCE
#include "http.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_FILES 64
#define MAX_FILE_SIZE (200 * 1024)  // must fit in the send queue with its headers

typedef struct {
    char path[256];      // URL path, like /assets/index-abc.js
    const char *type;    // Content-Type
    int immutable;       // hashed asset names never change: cache them for a year
    const char *data;
    size_t size;
} File;

static File files[MAX_FILES];
static int nfiles = 0;

static const char *type_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".wasm")) return "application/wasm";
    return "application/octet-stream";
}

static void load_file(const char *disk_path, const char *url_path) {
    if (nfiles == MAX_FILES) return;
    FILE *f = fopen(disk_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0 || size > MAX_FILE_SIZE) {
        fprintf(stderr, "skipping %s: bigger than %d KB\n", disk_path, MAX_FILE_SIZE / 1024);
        fclose(f);
        return;
    }
    File *out = &files[nfiles];
    char *data = malloc((size_t)size + 1); // start-up only; never freed, lives as long as the server
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); return; }
    out->data = data;
    fclose(f);
    out->size = (size_t)size;
    memcpy(out->path, url_path, strlen(url_path) + 1); // url_path is under 256 bytes (checked in load_dir)
    out->type = type_of(url_path);
    out->immutable = strncmp(url_path, "/assets/", 8) == 0;
    nfiles++;
}

static void load_dir(const char *dir, const char *url_prefix, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char disk[1024], url[256];
        int dl = snprintf(disk, sizeof disk, "%s/%s", dir, e->d_name);
        int ul = snprintf(url, sizeof url, "%s/%s", url_prefix, e->d_name);
        if (dl < 0 || ul < 0 || (size_t)dl >= sizeof disk || (size_t)ul >= sizeof url) continue; // name too long
        struct stat st;
        if (stat(disk, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 1) load_dir(disk, url, depth + 1); }
        else if (S_ISREG(st.st_mode)) load_file(disk, url);
    }
    closedir(d);
}

static File *find(const char *path) {
    for (int i = 0; i < nfiles; i++)
        if (!strcmp(files[i].path, path)) return &files[i];
    return NULL;
}

#ifdef RELEASE
extern const EmbeddedFile embedded_files[];
extern const int embedded_count;
#endif

int http_load(const char *dir) {
#ifdef RELEASE
    (void)dir;
    (void)load_dir;
    for (int i = 0; i < embedded_count && nfiles < MAX_FILES; i++) {
        File *out = &files[nfiles++];
        snprintf(out->path, sizeof out->path, "%s", embedded_files[i].path);
        out->type = type_of(out->path);
        out->immutable = strncmp(out->path, "/assets/", 8) == 0;
        out->data = (const char *)embedded_files[i].data;
        out->size = embedded_files[i].size;
    }
#else
    load_dir(dir, "", 0);
#endif
    return find("/index.html") ? nfiles : -1;
}

static void reply(WsConn *c, const char *status, const char *type, const char *cache,
                  const char *body, size_t n, int head_only, int gzipped) {
    char hdr[512];
    int hl = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
        "Cache-Control: %s\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        status, type, n, gzipped ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" : "", cache);
    ws_send_raw(c, hdr, (size_t)hl);
    if (!head_only && n) ws_send_raw(c, body, n);
    c->close_after = 1;
}

void http_answer(WsConn *c) {
    char method[8], path[256];
    if (sscanf((char *)c->in, "%7s %255s", method, path) != 2) {
        reply(c, "400 Bad Request", "text/plain", "no-store", "bad request\n", 12, 0, 0);
        return;
    }
    int head = !strcmp(method, "HEAD");
    if (strcmp(method, "GET") && !head) {
        reply(c, "405 Method Not Allowed", "text/plain", "no-store", "GET only\n", 9, 0, 0);
        return;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0; // ignore ?query
    File *f = find(!strcmp(path, "/") ? "/index.html" : path);
    if (!f && !strchr(path, '.')) f = find("/index.html"); // app routes like /ride -> the app
    if (!f) {
        reply(c, "404 Not Found", "text/plain", "no-store", "not found\n", 10, head, 0);
        return;
    }
    // Pre-compressed copy (made once by `make page`): send it if the browser accepts gzip.
    // Compression costs nothing here: it was done at build time, not on every request.
    char gzpath[300];
    File *gz = NULL;
    if (strcasestr((char *)c->in, "gzip") && (size_t)snprintf(gzpath, sizeof gzpath, "%s.gz", f->path) < sizeof gzpath)
        gz = find(gzpath);
    File *send = gz ? gz : f;
    reply(c, "200 OK", f->type, f->immutable ? "public, max-age=31536000, immutable" : "no-cache",
          send->data, send->size, head, gz != NULL);
}
