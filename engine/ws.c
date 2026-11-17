// ws.c - WebSocket protocol only. Knows nothing about cars or rides.
#define _GNU_SOURCE
#include "ws.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>

static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t buf[256] = {0};
    if (total > sizeof buf) return;
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[off + 4 * i] << 24 | (uint32_t)buf[off + 4 * i + 1] << 16 |
                   (uint32_t)buf[off + 4 * i + 2] << 8 | buf[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = h[i] >> 24; out[4 * i + 1] = h[i] >> 16;
        out[4 * i + 2] = h[i] >> 8; out[4 * i + 3] = h[i];
    }
}

static void base64(const uint8_t *in, size_t len, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static int g_epfd = -1;
void ws_set_epoll(int epfd) { g_epfd = epfd; }

static void want_write(WsConn *c, int on) { // ask epoll to wake us when the socket has room
    if (g_epfd < 0) return;
    struct epoll_event ev = {.events = EPOLLIN | (on ? EPOLLOUT : 0), .data.ptr = c};
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

int ws_flush(WsConn *c) {
    size_t done = 0;
    while (done < c->out_len) {
        ssize_t w = send(c->fd, c->out + done, c->out_len - done, MSG_NOSIGNAL);
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break; // socket full: try later
        return -1;
    }
    memmove(c->out, c->out + done, c->out_len - done);
    c->out_len -= done;
    want_write(c, c->out_len > 0);
    return 0;
}

// Put whole pieces in the queue, then try to send right away.
static int queue(WsConn *c, const void *a, size_t an, const void *b, size_t bn) {
    if (c->dead) return -1;
    if (c->out_len + an + bn > WS_OUTBUF) return -1; // never queue half a message
    memcpy(c->out + c->out_len, a, an); c->out_len += an;
    if (bn) { memcpy(c->out + c->out_len, b, bn); c->out_len += bn; }
    if (ws_flush(c) < 0) c->dead = 1;
    return 0;
}

static int send_frame(WsConn *c, uint8_t opcode, const void *data, size_t n) {
    uint8_t hdr[10];
    size_t hl;
    hdr[0] = 0x80 | opcode;
    if (n < 126) { hdr[1] = (uint8_t)n; hl = 2; }
    else if (n <= 65535) { hdr[1] = 126; hdr[2] = (uint8_t)(n >> 8); hdr[3] = (uint8_t)n; hl = 4; }
    else { hdr[1] = 127; for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i)); hl = 10; }
    return queue(c, hdr, hl, data, n);
}

int ws_send_text(WsConn *c, const char *msg, size_t n) { return send_frame(c, 0x1, msg, n); }
int ws_send_binary(WsConn *c, const void *data, size_t n) { return send_frame(c, 0x2, data, n); }
int ws_send_raw(WsConn *c, const void *data, size_t n) { return queue(c, data, n, NULL, 0); }

int ws_is_upgrade(const WsConn *c) {
    return strcasestr((const char *)c->in, "Upgrade: websocket") != NULL;
}

int ws_handshake(WsConn *c) {
    c->in[c->len < WS_INBUF ? c->len : WS_INBUF - 1] = 0;
    char *end = strstr((char *)c->in, "\r\n\r\n");
    if (!end) return 0;
    char *k = strcasestr((char *)c->in, "Sec-WebSocket-Key:");
    if (!k) return -1;
    k += 18;
    while (*k == ' ') k++;
    char key[128];
    size_t kl = 0;
    while (k[kl] && k[kl] != '\r' && kl < 60) { key[kl] = k[kl]; kl++; }
    memcpy(key + kl, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    uint8_t hash[20];
    char accept[32], resp[256];
    sha1((uint8_t *)key, kl + 36, hash);
    base64(hash, 20, accept);
    int n = snprintf(resp, sizeof resp,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (ws_send_raw(c, resp, (size_t)n) < 0) return -1;
    size_t used = (size_t)(end + 4 - (char *)c->in);
    memmove(c->in, c->in + used, c->len - used);
    c->len -= used;
    c->upgraded = 1;
    return 1;
}

int ws_read(WsConn *c, void (*on_text)(WsConn *c, char *msg, size_t n)) {
    for (;;) {
        if (c->len < 2) return 0;
        uint8_t op = c->in[0] & 0x0F;
        size_t plen = c->in[1] & 0x7F, hl = 2;
        if (plen == 126) { if (c->len < 4) return 0; plen = (size_t)c->in[2] << 8 | c->in[3]; hl = 4; }
        else if (plen == 127) return -1;
        if (!(c->in[1] & 0x80)) return -1; // browsers always mask
        if (hl + 4 + plen > WS_INBUF) return -1;
        if (c->len < hl + 4 + plen) return 0;
        uint8_t *mask = c->in + hl, *data = c->in + hl + 4;
        for (size_t j = 0; j < plen; j++) data[j] ^= mask[j & 3];
        if (op == 0x8) return -1;
        if (op == 0x9) send_frame(c, 0xA, data, plen);
        else if (op == 0x1) on_text(c, (char *)data, plen);
        size_t used = hl + 4 + plen;
        memmove(c->in, c->in + used, c->len - used);
        c->len -= used;
    }
}
