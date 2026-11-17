// gen_protocol.c - reads protocol.def, writes protocol.h (C) and protocol.ts (TypeScript).
// usage: gen_protocol protocol.def out.h out.ts PREFIX
// PREFIX keeps engine and app names apart: ENG -> ENG_PORT, eng_parse_join(), eng_write_stats()
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX 64

typedef struct { char name[64], type[256]; } Field;
typedef struct { char kind[8], name[64], word[64]; Field f[MAX]; int nf; } Item; // word = command prefix

static Item items[MAX];
static int nitems = 0;
static char consts[MAX][2][64];
static int nconsts = 0;
static char P[16], p[16];   // PREFIX and prefix

static void die(int line, const char *msg) { fprintf(stderr, "protocol.def:%d: %s\n", line, msg); exit(1); }

static int is_type(const char *t) {
    return !strcmp(t, "int") || !strcmp(t, "f1") || !strcmp(t, "f2") || !strcmp(t, "int3") || !strncmp(t, "enum(", 5);
}

static void parse(const char *path) {
    FILE *in = fopen(path, "r");
    if (!in) { perror(path); exit(1); }
    char line[1024];
    int ln = 0;
    while (fgets(line, sizeof line, in)) {
        ln++;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *tok[MAX + 3];
        int nt = 0;
        for (char *t = strtok(line, " \t\r\n"); t && nt < MAX + 3; t = strtok(NULL, " \t\r\n")) tok[nt++] = t;
        if (nt == 0) continue;
        if (!strcmp(tok[0], "const")) {
            if (nt != 3 || nconsts == MAX) die(ln, "const NAME VALUE");
            snprintf(consts[nconsts][0], 64, "%s", tok[1]);
            snprintf(consts[nconsts][1], 64, "%s", tok[2]);
            nconsts++;
            continue;
        }
        int is_cmd = !strcmp(tok[0], "cmd"), is_msg = !strcmp(tok[0], "msg");
        if (!is_cmd && !is_msg) die(ln, "lines start with const, cmd or msg");
        if (nitems == MAX) die(ln, "too many items");
        Item *it = &items[nitems++];
        memset(it, 0, sizeof *it);
        snprintf(it->kind, sizeof it->kind, "%s", tok[0]);
        if (nt < 2) die(ln, "missing name");
        snprintf(it->name, sizeof it->name, "%s", tok[1]);
        int first = 2;
        if (is_cmd) {
            if (nt < 3) die(ln, "cmd NAME WORD fields...");
            snprintf(it->word, sizeof it->word, "%s", tok[2]);
            first = 3;
        }
        for (int i = first; i < nt; i++) {
            char *colon = strchr(tok[i], ':');
            if (!colon) die(ln, "fields look like name:type");
            *colon = 0;
            if (!is_type(colon + 1)) die(ln, "unknown type");
            if (is_cmd && strcmp(colon + 1, "int")) die(ln, "commands only take int fields");
            Field *f = &it->f[it->nf++];
            snprintf(f->name, sizeof f->name, "%s", tok[i]);
            snprintf(f->type, sizeof f->type, "%s", colon + 1);
        }
    }
    fclose(in);
}

static void write_c(const char *path) {
    FILE *o = fopen(path, "w");
    if (!o) { perror(path); exit(1); }
    fprintf(o, "// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.\n");
    fprintf(o, "#pragma once\n\n");
    for (int i = 0; i < nconsts; i++) fprintf(o, "#define %s_%s %s\n", P, consts[i][0], consts[i][1]);
    // The browser build (no C library) only needs the numbers above.
    fprintf(o, "\n#ifndef PROTO_CONSTANTS_ONLY\n#include <stddef.h>\n#include <stdio.h>\n#include <string.h>\n\n");
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (!strcmp(it->kind, "cmd")) {
            // parse_quote(s, &px, ...) -> 1 if s is exactly that command
            fprintf(o, "// browser -> server: \"%s", it->word);
            for (int k = 0; k < it->nf; k++) fprintf(o, ",%s", it->f[k].name);
            fprintf(o, "\"\nstatic inline int %s_parse_%s(const char *s", p, it->name);
            for (int k = 0; k < it->nf; k++) fprintf(o, ", int *%s", it->f[k].name);
            if (it->nf == 0) { fprintf(o, ") { return strcmp(s, \"%s\") == 0; }\n\n", it->word); continue; }
            fprintf(o, ") {\n    char extra;\n    return sscanf(s, \"%s", it->word);
            for (int k = 0; k < it->nf; k++) fprintf(o, ",%%d");
            fprintf(o, "%%c\"");
            for (int k = 0; k < it->nf; k++) fprintf(o, ", %s", it->f[k].name);
            fprintf(o, ", &extra) == %d; // nothing allowed after the last number\n}\n\n", it->nf);
        } else {
            // write_ride(buf, n, fields...) -> length of the JSON written
            fprintf(o, "// server -> browser: {\"t\":\"%s\",...}\nstatic inline int %s_write_%s(char *b, size_t n", it->name, p, it->name);
            for (int k = 0; k < it->nf; k++) {
                const char *t = it->f[k].type;
                if (!strcmp(t, "int")) fprintf(o, ", int %s", it->f[k].name);
                else if (!strcmp(t, "int3")) fprintf(o, ", const int %s[3]", it->f[k].name);
                else if (t[0] == 'f') fprintf(o, ", double %s", it->f[k].name);
                else fprintf(o, ", const char *%s", it->f[k].name);
            }
            fprintf(o, ") {\n    return snprintf(b, n, \"{\\\"t\\\":\\\"%s\\\"", it->name);
            for (int k = 0; k < it->nf; k++) {
                const char *t = it->f[k].type, *nm = it->f[k].name;
                if (!strcmp(t, "int")) fprintf(o, ",\\\"%s\\\":%%d", nm);
                else if (!strcmp(t, "f1")) fprintf(o, ",\\\"%s\\\":%%.1f", nm);
                else if (!strcmp(t, "f2")) fprintf(o, ",\\\"%s\\\":%%.2f", nm);
                else if (!strcmp(t, "int3")) fprintf(o, ",\\\"%s\\\":[%%d,%%d,%%d]", nm);
                else fprintf(o, ",\\\"%s\\\":\\\"%%s\\\"", nm);
            }
            fprintf(o, "}\"");
            for (int k = 0; k < it->nf; k++) {
                if (!strcmp(it->f[k].type, "int3")) fprintf(o, ", %s[0], %s[1], %s[2]", it->f[k].name, it->f[k].name, it->f[k].name);
                else fprintf(o, ", %s", it->f[k].name);
            }
            fprintf(o, ");\n}\n\n");
        }
    }
    fprintf(o, "#endif\n");
    fclose(o);
}

static void write_ts(const char *path) {
    FILE *o = fopen(path, "w");
    if (!o) { perror(path); exit(1); }
    fprintf(o, "// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.\n\n");
    for (int i = 0; i < nconsts; i++) fprintf(o, "export const %s = %s;\n", consts[i][0], consts[i][1]);
    fprintf(o, "\n");
    char union_list[2048] = "";
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (strcmp(it->kind, "msg")) continue;
        char tname[80];
        snprintf(tname, sizeof tname, "%c%sMsg", toupper((unsigned char)it->name[0]), it->name + 1);
        fprintf(o, "export type %s = {\n  t: '%s';\n", tname, it->name);
        for (int k = 0; k < it->nf; k++) {
            const char *t = it->f[k].type;
            fprintf(o, "  %s: ", it->f[k].name);
            if (!strcmp(t, "int3")) fprintf(o, "[number, number, number]");
            else if (!strncmp(t, "enum(", 5)) { // enum(a|b) -> 'a' | 'b'
                const char *p = t + 5;
                int firstv = 1;
                while (*p && *p != ')') {
                    const char *e = p;
                    while (*e && *e != '|' && *e != ')') e++;
                    fprintf(o, "%s'%.*s'", firstv ? "" : " | ", (int)(e - p), p);
                    firstv = 0;
                    p = *e == '|' ? e + 1 : e;
                }
            } else fprintf(o, "number");
            fprintf(o, ";\n");
        }
        fprintf(o, "};\n\n");
        if (union_list[0]) strncat(union_list, " | ", sizeof union_list - strlen(union_list) - 1);
        strncat(union_list, tname, sizeof union_list - strlen(union_list) - 1);
    }
    fprintf(o, "export type ServerMsg = %s;\n\n", union_list);
    fprintf(o, "// Builders for every browser -> server command.\nexport const cmd = {\n");
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        if (strcmp(it->kind, "cmd")) continue;
        fprintf(o, "  %s: (", it->name);
        for (int k = 0; k < it->nf; k++) fprintf(o, "%s%s: number", k ? ", " : "", it->f[k].name);
        fprintf(o, ") => `%s", it->word);
        for (int k = 0; k < it->nf; k++) fprintf(o, ",${Math.round(%s)}", it->f[k].name);
        fprintf(o, "`,\n");
    }
    fprintf(o, "};\n");
    fclose(o);
}

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s protocol.def out.h out.ts PREFIX\n", argv[0]); return 1; }
    snprintf(P, sizeof P, "%s", argv[4]);
    for (int i = 0; P[i]; i++) p[i] = (char)tolower((unsigned char)P[i]);
    parse(argv[1]);
    write_c(argv[2]);
    write_ts(argv[3]);
    printf("protocol: %d constants, %d messages/commands -> %s, %s\n", nconsts, nitems, argv[2], argv[3]);
    return 0;
}
