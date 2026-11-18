/*
 * common/frame.h — 4-byte length framing + field splitting, shared by client, server and tools.
 *
 * TCP is a byte stream: one send() may arrive as several recv() calls, and several send()s
 * may arrive glued together in one recv() (Lec04, "byte stream problem"). We use solution 3,
 * length prefixing: every message is [uint32 LENGTH, network byte order][LENGTH bytes payload].
 * See docs/protocol.md §1–2.
 *
 * Usage: after each frame_recv(), call frame_next() in a loop until it returns 0 (drain every
 * complete frame already in the buffer) before calling frame_recv() again.
 *
 * Owner: Nghĩa (writes), both (use). Interface fixed in CLAUDE.md — change only by agreement.
 */
#ifndef CARO_FRAME_H
#define CARO_FRAME_H

#include <stddef.h>

#include "proto.h"   /* FRAME_MAX_PAYLOAD (16384) */

/* One receive buffer per connection: holds a partial frame until it is complete. */
typedef struct { char buf[4 + FRAME_MAX_PAYLOAD]; size_t len; } FrameBuf;

void frame_init(FrameBuf *fb);
int  frame_send(int fd, const char *payload);              /* loops on partial send; 0 ok, -1 error */
int  frame_recv(FrameBuf *fb, int fd);                     /* one recv(); >0 bytes, 0 peer closed, -1 error */
int  frame_next(FrameBuf *fb, char *out, size_t out_cap);  /* 1 got message, 0 need more, -1 bad frame */
int  msg_split(char *msg, char *fields[], int max_fields); /* split on '|' in place, returns count */

#endif /* CARO_FRAME_H */
