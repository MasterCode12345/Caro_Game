/*
 * tools/test_frame.c — unit tests for common/frame.c. Built as build/test_frame, run by `make test`.
 * Owner: Nghĩa.
 *
 * socketpair(AF_UNIX, SOCK_STREAM) gives two connected stream sockets inside one process. They
 * behave like a TCP connection for our purpose — a byte stream with no message boundaries — but
 * need no network and no server. Each test writes raw bytes on one end exactly as it wants (one
 * byte at a time, several frames at once, broken headers) and reads them back on the other end
 * with frame_recv() / frame_next(), the same way the server and the client do.
 *
 * Prints PASS/FAIL per test; exit code 1 if any test fails.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "frame.h"

/* A test gets the writing end and the reading end of a fresh socket pair. It returns NULL on
 * success, otherwise a short description of what went wrong. It may leave a remark in note. */
typedef const char *(*TestFn)(int wr, int rd);

static char out[FRAME_MAX_PAYLOAD + 1];   /* frame_next() output, big enough for any frame */
static char note[96];

/* ---- helpers: build and write raw bytes, bypassing frame_send() ---------------------------- */

static int write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t r = write(fd, p, len);
        if (r < 0)
            return -1;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

/* Writes [LENGTH][payload] into buf (which must hold 4 + strlen(payload) bytes); returns the size. */
static size_t make_frame(char *buf, const char *payload)
{
    size_t n = strlen(payload);
    uint32_t len_be = htonl((uint32_t)n);
    memcpy(buf, &len_be, 4);
    memcpy(buf + 4, payload, n);
    return 4 + n;
}

static int write_header(int fd, uint32_t length)
{
    uint32_t len_be = htonl(length);
    return write_all(fd, &len_be, 4);
}

/* ---- tests ---------------------------------------------------------------------------------- */

/* a) Cut frame, worst case: every byte arrives in its own recv(). */
static const char *test_byte_by_byte(int wr, int rd)
{
    const char *msg = "LOGIN|nghia|123456";
    char frame[64];
    size_t len = make_frame(frame, msg);
    FrameBuf fb;
    frame_init(&fb);

    for (size_t i = 0; i < len; i++) {
        if (write_all(wr, frame + i, 1) < 0)
            return "write() failed";
        if (frame_recv(&fb, rd) != 1)
            return "frame_recv() did not return exactly 1 byte";
        int r = frame_next(&fb, out, sizeof out);
        if (i + 1 < len && r != 0)
            return "frame_next() != 0 before the last byte arrived";
        if (i + 1 == len && r != 1)
            return "frame_next() != 1 after the last byte arrived";
    }
    if (strcmp(out, msg) != 0)
        return "payload differs";
    if (fb.len != 0 || frame_next(&fb, out, sizeof out) != 0)
        return "bytes left in the buffer after the frame";
    return NULL;
}

/* b) Glued frames: three frames written in one write(), received by one recv(). */
static const char *test_three_glued(int wr, int rd)
{
    const char *msgs[3] = { "PING", "LOGIN|nghia|123456", "MOVE|5|7|7" };
    char buf[128];
    size_t len = 0;
    for (int i = 0; i < 3; i++)
        len += make_frame(buf + len, msgs[i]);

    if (write_all(wr, buf, len) < 0)
        return "write() failed";
    FrameBuf fb;
    frame_init(&fb);
    if (frame_recv(&fb, rd) != (int)len)
        return "the three frames did not arrive in one recv()";

    for (int i = 0; i < 3; i++) {
        if (frame_next(&fb, out, sizeof out) != 1)
            return "frame_next() did not return 1 for each of the 3 frames";
        if (strcmp(out, msgs[i]) != 0)
            return "wrong payload or wrong order";
    }
    if (frame_next(&fb, out, sizeof out) != 0 || fb.len != 0)
        return "frame_next() should return 0 once the buffer is drained";
    return NULL;
}

/* c) Malformed headers are rejected as soon as the 4 header bytes are in. */
static const char *test_bad_length(int wr, int rd)
{
    FrameBuf fb;

    frame_init(&fb);
    if (write_header(wr, 0) < 0 || frame_recv(&fb, rd) != 4)
        return "could not write/read the LENGTH 0 header";
    if (frame_next(&fb, out, sizeof out) != -1)
        return "LENGTH 0 not rejected";

    frame_init(&fb);
    if (write_header(wr, FRAME_MAX_PAYLOAD + 1) < 0 || frame_recv(&fb, rd) != 4)
        return "could not write/read the LENGTH 16385 header";
    if (frame_next(&fb, out, sizeof out) != -1)
        return "LENGTH 16385 not rejected";

    /* boundary: 16384 is legal, so a lone header means "wait for the payload" */
    frame_init(&fb);
    if (write_header(wr, FRAME_MAX_PAYLOAD) < 0 || frame_recv(&fb, rd) != 4)
        return "could not write/read the LENGTH 16384 header";
    if (frame_next(&fb, out, sizeof out) != 0)
        return "LENGTH 16384 should be accepted (return 0: payload not here yet)";
    return NULL;
}

/* d) msg_split on the LIST_PLAYERS example of docs/protocol.md §2, plus edge cases. */
static const char *test_msg_split(int wr, int rd)
{
    (void)wr;
    (void)rd;
    char *f[MAX_FIELDS];

    char msg[] = "200|LIST_PLAYERS|2|han,1016,IDLE|minh,980,IN_GAME";
    const char *want[5] = { "200", "LIST_PLAYERS", "2", "han,1016,IDLE", "minh,980,IN_GAME" };
    if (msg_split(msg, f, MAX_FIELDS) != 5)
        return "LIST_PLAYERS example: expected 5 fields";
    for (int i = 0; i < 5; i++)
        if (strcmp(f[i], want[i]) != 0)
            return "LIST_PLAYERS example: wrong field content";

    char empty_mid[] = "a||b";
    if (msg_split(empty_mid, f, MAX_FIELDS) != 3 || strcmp(f[1], "") != 0)
        return "\"a||b\" should give 3 fields, the middle one empty";

    char capped[] = "a|b|c|d";
    if (msg_split(capped, f, 2) != 2 || strcmp(f[0], "a") != 0 || strcmp(f[1], "b|c|d") != 0)
        return "max_fields = 2 on \"a|b|c|d\" should give \"a\" and \"b|c|d\"";
    return NULL;
}

/* e) frame_send(): round trip, and invalid payloads never reach the wire. */
static const char *test_frame_send(int wr, int rd)
{
    static char too_big[FRAME_MAX_PAYLOAD + 2];
    memset(too_big, 'a', FRAME_MAX_PAYLOAD + 1);
    too_big[FRAME_MAX_PAYLOAD + 1] = '\0';                  /* 16385 characters */

    if (frame_send(wr, "") != -1)
        return "empty payload accepted";
    if (frame_send(wr, too_big) != -1)
        return "16385-byte payload accepted";
    char probe;
    if (recv(rd, &probe, 1, MSG_DONTWAIT) != -1)
        return "a rejected payload still put bytes on the wire";

    if (frame_send(wr, "PING") != 0)
        return "frame_send(\"PING\") failed";
    FrameBuf fb;
    frame_init(&fb);
    if (frame_recv(&fb, rd) != 8 || frame_next(&fb, out, sizeof out) != 1)
        return "PING frame not received as 4 + 4 bytes";
    if (strcmp(out, "PING") != 0)
        return "PING payload differs";
    return NULL;
}

/* f) A max-size frame (4 + 16384 bytes) is larger than a socket buffer (8 KB for AF_UNIX on
 *    macOS), so send() cannot finish until the reader drains it: a child process sends while we
 *    read. The frame arrives cut into several recv() calls, as it would over a real network. */
static const char *test_max_size(int wr, int rd)
{
    static char big[FRAME_MAX_PAYLOAD + 1];
    for (int i = 0; i < FRAME_MAX_PAYLOAD; i++)
        big[i] = (char)('a' + i % 26);
    big[FRAME_MAX_PAYLOAD] = '\0';

    pid_t pid = fork();
    if (pid < 0)
        return "fork() failed";
    if (pid == 0)
        _exit(frame_send(wr, big) == 0 ? 0 : 1);

    FrameBuf fb;
    frame_init(&fb);
    int r, recvs = 0;
    while ((r = frame_next(&fb, out, sizeof out)) == 0) {
        if (frame_recv(&fb, rd) <= 0)    /* SO_RCVTIMEO: a bug fails the test, never hangs it */
            break;
        recvs++;
    }
    if (r != 1)
        kill(pid, SIGKILL);              /* the child may still be blocked in send() */
    int status;
    if (waitpid(pid, &status, 0) < 0)
        return "waitpid() failed";

    if (r != 1)
        return "max-size frame not received";
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return "frame_send() failed in the child";
    if (strcmp(out, big) != 0)
        return "payload differs";
    snprintf(note, sizeof note, "arrived in %d recv() calls", recvs);
    return NULL;
}

/* ---- runner --------------------------------------------------------------------------------- */

static const struct {
    const char *name;
    TestFn fn;
} tests[] = {
    { "a) one frame sent one byte at a time",            test_byte_by_byte },
    { "b) three frames glued in one write()",            test_three_glued },
    { "c) LENGTH 0 and 16385 rejected, 16384 accepted",  test_bad_length },
    { "d) msg_split",                                    test_msg_split },
    { "e) frame_send round trip, rejects empty/oversize", test_frame_send },
    { "f) frame_send of a max-size frame",               test_max_size },
};

int main(void)
{
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal(SIGPIPE)");
        return 1;
    }

    int n = (int)(sizeof tests / sizeof tests[0]);
    int failed = 0;
    for (int i = 0; i < n; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            perror("socketpair");
            return 1;
        }
        /* A frame_recv() that waits forever would hang `make test`; fail after 2 s instead. */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        if (setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0) {
            perror("setsockopt(SO_RCVTIMEO)");
            return 1;
        }

        /* Name first and flushed: if a bug crashes the test, the output still shows which one. */
        printf("%-50s ", tests[i].name);
        fflush(stdout);
        note[0] = '\0';
        const char *err = tests[i].fn(sv[0], sv[1]);
        close(sv[0]);
        close(sv[1]);

        if (err != NULL) {
            printf("FAIL: %s\n", err);
            failed++;
        } else if (note[0] != '\0') {
            printf("PASS (%s)\n", note);
        } else {
            printf("PASS\n");
        }
    }
    printf("test_frame: %d/%d passed\n", n - failed, n);
    return failed == 0 ? 0 : 1;
}
