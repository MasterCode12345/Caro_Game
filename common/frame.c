/*
 * common/frame.c — length-prefixed framing over TCP + field splitting (docs/protocol.md §1–2).
 * Owner: Nghĩa (writes), both (use).
 *
 * THE BYTE STREAM PROBLEM (Lec04)
 *   TCP delivers a stream of bytes, not messages. It keeps the order, but not the boundaries:
 *   - one send() of 18 bytes may arrive as recv() = 5 bytes, then 13 bytes  ("cut" frame);
 *   - three quick send()s may arrive in a single recv()                      ("glued" frames).
 *   So "one recv() = one message" is wrong, and the receiver needs a rule to find the end of
 *   each message.
 *
 * WHY LENGTH PREFIXING (solution 3)
 *   1. Fixed-size messages: simple, but our messages range from "PING" (4 bytes) to a REPLAY of
 *      hundreds of moves; every message would have to be as big as the biggest one.
 *   2. A delimiter such as '\n': the receiver scans every byte, and the delimiter must never
 *      appear inside the data.
 *   3. Length prefix: each frame starts with its payload length, so the receiver knows exactly
 *      how many bytes to wait for before the message is complete. No scanning, any content.
 *   Frame = [LENGTH: uint32, network byte order][PAYLOAD: LENGTH bytes], 1 <= LENGTH <= 16384.
 *   Network byte order (big-endian, htonl/ntohl) makes a little-endian Mac and any other machine
 *   read the same number. The upper bound protects the receiver: a peer cannot make us wait
 *   for, or buffer, an arbitrary amount of data. LENGTH 0 or > 16384 is a malformed frame.
 *
 * RECEIVE MODEL
 *   One FrameBuf per connection. When select() (server, cli) or g_io_add_watch() (GTK client)
 *   says the socket is readable, call frame_recv() ONCE — one recv() will not block, a second
 *   one might — then call frame_next() until it returns 0. A cut frame simply stays in the
 *   buffer until the next frame_recv() brings the rest.
 */
#include "frame.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>    /* htonl, ntohl */

/* Linux: MSG_NOSIGNAL makes send() on a closed connection fail with EPIPE instead of raising
 * SIGPIPE (which would kill the process). macOS has no such flag, so it becomes 0 there and we
 * rely on every main() calling signal(SIGPIPE, SIG_IGN) — see CLAUDE.md "Platform notes". */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define HEADER_SIZE 4     /* the uint32 LENGTH field */

void frame_init(FrameBuf *fb)
{
    fb->len = 0;
}

/* send() may send fewer bytes than asked (Lec04): when the kernel send buffer is nearly full,
 * or when a signal interrupts it. Loop until everything is out. */
static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR)
                continue;          /* interrupted before anything was sent: just retry */
            return -1;             /* EPIPE, ECONNRESET, or EAGAIN after SO_SNDTIMEO (server) */
        }
        sent += (size_t)r;
    }
    return 0;
}

int frame_send(int fd, const char *payload)
{
    if (payload == NULL)
        return -1;
    size_t n = strlen(payload);
    if (n == 0 || n > FRAME_MAX_PAYLOAD)
        return -1;                 /* would be a malformed frame: never put it on the wire */

    /* Header and payload in ONE buffer: one send() call in the normal case, and no window in
     * which the peer has a header without its payload because of our own code. */
    char frame[HEADER_SIZE + FRAME_MAX_PAYLOAD];
    uint32_t len_be = htonl((uint32_t)n);
    memcpy(frame, &len_be, HEADER_SIZE);
    memcpy(frame + HEADER_SIZE, payload, n);
    return send_all(fd, frame, HEADER_SIZE + n);
}

int frame_recv(FrameBuf *fb, int fd)
{
    size_t room = sizeof fb->buf - fb->len;
    /* After frame_next() is drained the buffer holds at most one incomplete frame, whose size is
     * < HEADER_SIZE + FRAME_MAX_PAYLOAD = sizeof buf, so room >= 1. room == 0 means the caller
     * skipped the drain. */
    if (room == 0)
        return -1;

    ssize_t r;
    do {
        r = recv(fd, fb->buf + fb->len, room, 0);
    } while (r < 0 && errno == EINTR);   /* a signal is not an error: same single read, retried */

    if (r <= 0)
        return (int)r;             /* 0: peer closed the connection (FIN); -1: error */
    fb->len += (size_t)r;
    return (int)r;
}

int frame_next(FrameBuf *fb, char *out, size_t out_cap)
{
    if (fb->len < HEADER_SIZE)
        return 0;                  /* not even the 4-byte header yet: wait for more bytes */

    uint32_t len_be;
    memcpy(&len_be, fb->buf, HEADER_SIZE);   /* memcpy, not a cast: buf may be unaligned */
    uint32_t n = ntohl(len_be);

    /* Checked as soon as the header is here, so a bad header ("/huge" in tools/cli) is rejected
     * at once instead of waiting for 20000 bytes that may never come. */
    if (n == 0 || n > FRAME_MAX_PAYLOAD)
        return -1;

    size_t frame_len = HEADER_SIZE + (size_t)n;
    if (fb->len < frame_len)
        return 0;                  /* cut frame: header known, payload incomplete */

    if (out_cap < (size_t)n + 1)
        return -1;                 /* caller's buffer too small for payload + '\0' */

    memcpy(out, fb->buf + HEADER_SIZE, n);
    out[n] = '\0';                 /* the payload has no '\0' on the wire */

    /* Glued frames: bytes after this frame belong to the next one. Move them to the front;
     * memmove because source and destination overlap. */
    memmove(fb->buf, fb->buf + frame_len, fb->len - frame_len);
    fb->len -= frame_len;
    return 1;
}

int msg_split(char *msg, char *fields[], int max_fields)
{
    if (msg == NULL || fields == NULL || max_fields <= 0)
        return 0;

    /* Each '|' is replaced by '\0' and the next field starts right after it, so the fields
     * point into msg itself: nothing is copied or allocated. "a||b" gives 3 fields, the middle
     * one empty; a payload without '|' is 1 field. */
    int count = 0;
    fields[count++] = msg;
    for (char *p = msg; *p != '\0'; p++) {
        if (*p != FIELD_SEP)
            continue;
        if (count == max_fields)
            break;                 /* full: the last field keeps the unsplit rest, '|' included */
        *p = '\0';
        fields[count++] = p + 1;
    }
    return count;
}
