/*
 * tools/cli.c — interactive line client for manual protocol testing (both members use it).
 * Usage: ./build/cli <host> <port>
 * Owner: Nghĩa.
 *
 * Every line typed on stdin is sent as ONE frame; every frame received is printed at once as
 * "<< payload". Lines starting with '/' are test commands that send deliberately cut, glued or
 * malformed frames, so the server's framing can be checked (docs/protocol.md §1):
 *   /bytes <text>              one frame sent one byte at a time, 50 ms apart   (cut frame)
 *   /burst <a> ;; <b> ;; <c>   several frames in a single send()               (glued frames)
 *   /zero                      a header with LENGTH 0                          (malformed)
 *   /huge                      a header announcing LENGTH 20000, no payload    (malformed)
 *   /help, /quit
 * Output: "<<" = received frame, ">>" = sent line (echoed only when stdin is not a terminal),
 * "--" = message from cli itself.
 *
 * I/O model (Lec05): one thread, select() on stdin AND the socket. Whichever is readable is
 * handled, so a frame from the server (a response or an unrequested event) is printed even while
 * the user is not typing, and typing never waits for the server.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "frame.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0       /* macOS: SIGPIPE is ignored in main() instead */
#endif

#define BYTES_DELAY_MS  50
#define HUGE_LENGTH     20000
#define BURST_MAX       64
#define LINE_MAX_LEN    (4 * FRAME_MAX_PAYLOAD)   /* a /burst line may carry several payloads */

static int interactive;      /* stdin is a terminal: do not echo the lines we send */

/* ---- connection ----------------------------------------------------------------------------- */

/* getaddrinfo() turns a host name or an IPv4/IPv6 literal into a list of addresses (Lec03);
 * try them in order until one connect() succeeds. */
static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;          /* IPv4 or IPv6, whatever the name resolves to */
    hints.ai_socktype = SOCK_STREAM;      /* TCP */

    struct addrinfo *res;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "cli: %s: %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1, saved_errno = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            char ip[INET6_ADDRSTRLEN], serv[16];
            if (getnameinfo(ai->ai_addr, ai->ai_addrlen, ip, sizeof ip, serv, sizeof serv,
                            NI_NUMERICHOST | NI_NUMERICSERV) == 0)
                printf("-- connected to %s port %s\n", ip, serv);
            break;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        fprintf(stderr, "cli: cannot connect to %s port %s: %s\n", host, port, strerror(saved_errno));
    return fd;
}

/* ---- raw sending: the test commands build frames by hand, bypassing frame_send() ------------ */

/* Same loop as in common/frame.c: send() may send only part of the buffer (Lec04). */
static int send_raw(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t r = send(fd, p, len, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

/* Writes [LENGTH][payload] at dst, returns the number of bytes written (4 + n). */
static size_t put_frame(char *dst, const char *payload, size_t n)
{
    uint32_t len_be = htonl((uint32_t)n);
    memcpy(dst, &len_be, 4);
    memcpy(dst + 4, payload, n);
    return 4 + n;
}

static void send_header_only(int fd, uint32_t length)
{
    uint32_t len_be = htonl(length);
    if (send_raw(fd, &len_be, 4) < 0)
        printf("-- send failed: %s\n", strerror(errno));
    else
        printf("-- sent a header with LENGTH %u and no payload "
               "(malformed: the server should close the connection)\n", (unsigned)length);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

/* /bytes <text>: the server gets the frame in 4 + n separate recv() calls. */
static void cmd_bytes(int fd, const char *text)
{
    size_t n = strlen(text);
    if (n == 0 || n > FRAME_MAX_PAYLOAD) {
        printf("-- /bytes needs 1..%d characters of text\n", FRAME_MAX_PAYLOAD);
        return;
    }
    static char frame[4 + FRAME_MAX_PAYLOAD];
    size_t len = put_frame(frame, text, n);
    struct timespec delay = { .tv_sec = 0, .tv_nsec = BYTES_DELAY_MS * 1000000L };

    for (size_t i = 0; i < len; i++) {
        if (send_raw(fd, frame + i, 1) < 0) {
            printf("-- send failed after %zu of %zu bytes: %s\n", i, len, strerror(errno));
            return;
        }
        nanosleep(&delay, NULL);   /* the pause makes each byte leave in its own TCP segment */
    }
    printf("-- sent 1 frame (%zu bytes) one byte at a time\n", len);
}

/* /burst a ;; b ;; c: each part (spaces trimmed) is one frame; all frames are packed into one
 * buffer and given to one send(), so they reach the server glued together. */
static void cmd_burst(int fd, char *spec)
{
    char *parts[BURST_MAX];
    int count = 0;
    for (char *p = spec;;) {
        char *sep = strstr(p, ";;");
        if (sep != NULL)
            *sep = '\0';
        if (count == BURST_MAX) {
            printf("-- /burst: at most %d frames\n", BURST_MAX);
            return;
        }
        parts[count++] = trim(p);
        if (sep == NULL)
            break;
        p = sep + 2;
    }

    size_t total = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(parts[i]);
        if (n == 0) {
            printf("-- /burst: frame %d is empty (use /zero to send LENGTH 0)\n", i + 1);
            return;
        }
        if (n > FRAME_MAX_PAYLOAD) {
            printf("-- /burst: frame %d is longer than %d bytes\n", i + 1, FRAME_MAX_PAYLOAD);
            return;
        }
        total += 4 + n;
    }

    char *buf = malloc(total);
    if (buf == NULL) {
        perror("malloc");
        return;
    }
    size_t off = 0;
    for (int i = 0; i < count; i++)
        off += put_frame(buf + off, parts[i], strlen(parts[i]));

    if (send_raw(fd, buf, total) < 0)
        printf("-- send failed: %s\n", strerror(errno));
    else
        printf("-- sent %d frames (%zu bytes) glued in one send()\n", count, total);
    free(buf);
}

static void print_help(void)
{
    printf("-- any line                 sent as one frame, e.g. PING or LOGIN|nghia|123456\n"
           "-- /bytes <text>            one frame, one byte every %d ms (cut frame)\n"
           "-- /burst <a> ;; <b> ;; ... several frames in one send() (glued frames)\n"
           "-- /zero                    header with LENGTH 0 (malformed)\n"
           "-- /huge                    header announcing LENGTH %d, no payload (malformed)\n"
           "-- /quit                    close the connection and exit\n",
           BYTES_DELAY_MS, HUGE_LENGTH);
}

/* "/bytes PING" -> "PING", "/bytes" -> "", any other line -> NULL. */
static char *args_of(char *line, const char *cmd)
{
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) != 0)
        return NULL;
    if (line[n] == '\0')
        return line + n;
    return line[n] == ' ' ? line + n + 1 : NULL;
}

/* Handles one input line (without its '\n'). Returns 1 for /quit, else 0. */
static int handle_line(int fd, char *line)
{
    char *args;
    if (line[0] == '\0')
        return 0;
    if (!interactive)
        printf(">> %s\n", line);

    if (line[0] != '/') {
        size_t n = strlen(line);
        if (n > FRAME_MAX_PAYLOAD)
            printf("-- line too long: %zu bytes, the maximum payload is %d\n", n, FRAME_MAX_PAYLOAD);
        else if (frame_send(fd, line) < 0)
            printf("-- send failed: %s\n", strerror(errno));
        return 0;
    }

    if (strcmp(line, "/quit") == 0)
        return 1;
    if (strcmp(line, "/help") == 0)
        print_help();
    else if (strcmp(line, "/zero") == 0)
        send_header_only(fd, 0);
    else if (strcmp(line, "/huge") == 0)
        send_header_only(fd, HUGE_LENGTH);
    else if ((args = args_of(line, "/bytes")) != NULL)
        cmd_bytes(fd, args);
    else if ((args = args_of(line, "/burst")) != NULL)
        cmd_burst(fd, args);
    else
        printf("-- unknown command %s (try /help)\n", line);
    return 0;
}

/* ---- stdin ------------------------------------------------------------------------------------ */

/* stdin is read with read() into our own buffer, not with fgets(): stdio reads ahead into its
 * hidden buffer, and select() on fd 0 cannot see lines waiting there (a pasted block of lines
 * would get stuck). One read() per select() wake-up, then every complete line is handled. */
static char inbuf[LINE_MAX_LEN + 1];
static size_t inlen;
static int discarding;       /* inside a line longer than LINE_MAX_LEN: drop it up to its '\n' */

/* Returns 0 to go on, 1 at end of input, -1 for /quit. */
static int on_stdin(int sock)
{
    ssize_t r = read(STDIN_FILENO, inbuf + inlen, LINE_MAX_LEN - inlen);
    if (r < 0) {
        if (errno == EINTR)
            return 0;
        perror("read(stdin)");
        return 1;
    }
    if (r == 0) {                          /* end of input: the last line may lack its '\n' */
        if (inlen > 0 && !discarding) {
            inbuf[inlen] = '\0';
            inlen = 0;
            if (handle_line(sock, inbuf))
                return -1;
        }
        return 1;
    }
    inlen += (size_t)r;

    char *start = inbuf, *end = inbuf + inlen, *nl;
    while ((nl = memchr(start, '\n', (size_t)(end - start))) != NULL) {
        *nl = '\0';
        if (discarding) {
            discarding = 0;
        } else {
            if (nl > start && nl[-1] == '\r')
                nl[-1] = '\0';             /* tolerate CRLF input */
            if (handle_line(sock, start))
                return -1;
        }
        start = nl + 1;
    }

    inlen = (size_t)(end - start);         /* keep the incomplete last line for the next read */
    memmove(inbuf, start, inlen);
    if (inlen == LINE_MAX_LEN) {
        printf("-- line longer than %d bytes, discarded\n", LINE_MAX_LEN);
        inlen = 0;
        discarding = 1;
    }
    return 0;
}

/* ---- main ------------------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        return 2;
    }
    char *endp;
    errno = 0;
    long port = strtol(argv[2], &endp, 10);
    if (errno != 0 || endp == argv[2] || *endp != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "cli: invalid port '%s' (1..65535)\n", argv[2]);
        return 2;
    }

    /* A send() after the server closed would raise SIGPIPE and kill us; ignored, send() just
     * fails with EPIPE and the next select() reports the closed socket. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal(SIGPIPE)");
        return 1;
    }
    /* Line-buffered even into a pipe or a file, so output lines keep their real order. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    interactive = isatty(STDIN_FILENO);

    int sock = connect_to(argv[1], argv[2]);
    if (sock < 0)
        return 1;
    if (interactive)
        printf("-- type a request, e.g. PING (/help for the test commands)\n");

    static FrameBuf fb;
    static char msg[FRAME_MAX_PAYLOAD + 1];
    frame_init(&fb);
    int stdin_open = 1, status = 0;

    for (;;) {
        /* select() modifies the set, so it is rebuilt on every iteration. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        if (stdin_open)
            FD_SET(STDIN_FILENO, &rfds);
        int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {   /* no timeout: wait for input */
            if (errno == EINTR)
                continue;
            perror("select");
            status = 1;
            break;
        }

        if (FD_ISSET(sock, &rfds)) {
            int r = frame_recv(&fb, sock);   /* exactly one recv(): select() said it won't block */
            if (r == 0) {
                printf("-- server closed the connection\n");
                break;
            }
            if (r < 0) {
                printf("-- connection lost: %s\n", strerror(errno));
                status = 1;
                break;
            }
            int k;
            while ((k = frame_next(&fb, msg, sizeof msg)) == 1)   /* drain every complete frame */
                printf("<< %s\n", msg);
            if (k < 0) {
                printf("-- malformed frame from the server, closing\n");
                status = 1;
                break;
            }
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            int r = on_stdin(sock);
            if (r < 0)
                break;                       /* /quit */
            if (r > 0) {
                /* End of input (Ctrl+D, or the end of a piped file). Half-close: shutdown(SHUT_WR)
                 * sends FIN, so the server's recv() returns 0 after our last request, yet we can
                 * still read its replies. We exit when the server closes its side. */
                stdin_open = 0;
                if (shutdown(sock, SHUT_WR) < 0) {
                    perror("shutdown");
                    status = 1;
                    break;
                }
            }
        }
    }

    close(sock);
    return status;
}
