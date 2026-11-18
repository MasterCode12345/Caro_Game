/*
 * common/frame.c — implementation of common/frame.h.
 *
 * SKELETON: every function is a stub. Stage C0 writes the real code (see CLAUDE.md Roadmap).
 *
 * TODO(C0):
 *   - #define MSG_NOSIGNAL 0 when the system lacks it (macOS); callers ignore SIGPIPE.
 *   - frame_send: 4-byte htonl() length + payload; loop on partial send(), retry on EINTR,
 *     use MSG_NOSIGNAL; reject an empty payload or one longer than FRAME_MAX_PAYLOAD.
 *   - frame_recv: one recv() into the free part of fb->buf; return >0, 0 (peer closed), -1.
 *   - frame_next: need 4 bytes -> ntohl() length; 0 or > FRAME_MAX_PAYLOAD -> -1;
 *     incomplete -> 0; complete -> copy payload to out, add '\0', memmove the rest, return 1.
 *   - msg_split: split on '|' in place, at most max_fields fields, return the count.
 *   - Comment the byte stream problem and why we chose length prefixing.
 *
 * Owner: Nghĩa.
 */
#include "frame.h"

void frame_init(FrameBuf *fb)
{
    (void)fb;   /* TODO(C0) */
}

int frame_send(int fd, const char *payload)
{
    (void)fd;
    (void)payload;
    return -1;  /* TODO(C0) */
}

int frame_recv(FrameBuf *fb, int fd)
{
    (void)fb;
    (void)fd;
    return -1;  /* TODO(C0) */
}

int frame_next(FrameBuf *fb, char *out, size_t out_cap)
{
    (void)fb;
    (void)out;
    (void)out_cap;
    return -1;  /* TODO(C0) */
}

int msg_split(char *msg, char *fields[], int max_fields)
{
    (void)msg;
    (void)fields;
    (void)max_fields;
    return -1;  /* TODO(C0) */
}
