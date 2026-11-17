// ws.h - tiny WebSocket layer: handshake, read frames, queued sending.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define WS_INBUF  4096
#define WS_OUTBUF (256 * 1024)  // per-connection send queue

typedef struct {
    int fd;
    int open;         // slot in use
    int upgraded;     // WebSocket handshake done
    int dead;         // queue overflowed: close this connection
    int close_after;  // plain HTTP: close once everything is sent
    size_t len;       // bytes waiting in `in`
    size_t out_len;   // bytes waiting in `out`
    uint8_t in[WS_INBUF];
    uint8_t out[WS_OUTBUF];
} WsConn;

void ws_set_epoll(int epfd);     // so the queue can ask "tell me when I can send more"

// Is this HTTP request asking to become a WebSocket? (call once the headers are in)
int ws_is_upgrade(const WsConn *c);

// Returns 1 = handshake done, 0 = need more bytes, -1 = bad request.
int ws_handshake(WsConn *c);

// Calls on_text for every complete text message. Returns -1 to close.
int ws_read(WsConn *c, void (*on_text)(WsConn *c, char *msg, size_t n));

// Sending never blocks and never sends half a message.
// Returns 0 = queued or sent, -1 = no room (the connection is too slow).
int ws_send_text(WsConn *c, const char *msg, size_t n);
int ws_send_binary(WsConn *c, const void *data, size_t n);
int ws_send_raw(WsConn *c, const void *data, size_t n);  // plain HTTP bytes

// Call when epoll says the socket can take more. Returns -1 on error.
int ws_flush(WsConn *c);
