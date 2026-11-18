/*
 * client/net.c — the only file that touches the socket. UI code never calls send()/recv().
 * Owner: Nghĩa.
 *
 * WHY NO THREAD IS NEEDED
 *   GTK runs one loop (the GLib main loop) that sleeps in poll() until something happens: a
 *   click, a redraw, a timer... g_io_add_watch() adds our socket to that same poll(). When the
 *   server sends bytes, poll() wakes up and calls on_socket_ready(), which does ONE recv() —
 *   it cannot block, data is already there — handles the complete frames and returns to the
 *   loop. The GUI never waits inside recv(), so it never freezes, and since everything runs in
 *   one thread there is no locking.
 *
 * The only blocking step is connect() (and getaddrinfo() for a host name). A blocking connect()
 * to an unreachable LAN address can wait ~75 s, so it is done non-blocking with a
 * CONNECT_TIMEOUT_SEC limit.
 *
 * SIGPIPE: main() ignores it before anything is sent (CLAUDE.md "Platform notes").
 *
 * TODO(C4): losing the server on any page, also while a response is pending: close every
 *           dialog (net_on_lost -> app_go_login), never crash, never hang.
 */
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include "app.h"
#include "fake.h"
#include "frame.h"

#define CONNECT_TIMEOUT_SEC  3
#define SEND_TIMEOUT_SEC     2    /* a server that stops reading cannot freeze the GUI in send() */

static int         sock_fd = -1;
static gboolean    connected;
static GIOChannel *channel;
static guint       watch_id;       /* g_io_add_watch() source */
static guint       lost_idle_id;   /* deferred net_on_lost() after a failed send */
static guint       generation;     /* +1 on every close: lets a running callback notice it */
static FrameBuf    rx;             /* bytes received but not yet a complete frame */
static char        conn_host[256];
static int         conn_port;

/* ---- connect ------------------------------------------------------------------------------------ */

/* Non-blocking connect(): returns at once with EINPROGRESS; select() for writability with a
 * timeout tells when the handshake is over, SO_ERROR tells whether it succeeded. The socket is
 * put back in blocking mode afterwards: the main loop only calls recv() when data is ready. */
static int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    if (connect(fd, addr, addrlen) < 0) {
        if (errno != EINPROGRESS)
            return -1;
        int rc;
        for (;;) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_SEC, .tv_usec = 0 };
            rc = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (rc < 0 && errno == EINTR)
                continue;
            break;
        }
        if (rc < 0)
            return -1;
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        int soerr = 0;
        socklen_t len = sizeof soerr;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0)
            return -1;
        if (soerr != 0) {
            errno = soerr;           /* e.g. ECONNREFUSED: nobody listens on that port */
            return -1;
        }
    }
    return fcntl(fd, F_SETFL, flags);
}

static gboolean on_socket_ready(GIOChannel *source, GIOCondition cond, gpointer data);

static void remember_peer(const char *host, int port)
{
    snprintf(conn_host, sizeof conn_host, "%s", host);
    conn_port = port;
}

gboolean net_connect(const char *host, int port, char **err_out)
{
    net_close();

    if (app.fake_mode) {             /* offline: no socket at all, fake.c answers */
        fake_start();
        connected = TRUE;
        remember_peer(host, port);
        return TRUE;
    }

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        *err_out = g_strdup_printf("không tìm thấy máy '%s' (%s)", host, gai_strerror(rc));
        return FALSE;
    }

    int fd = -1, saved_errno = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            saved_errno = errno;
            continue;
        }
        if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        *err_out = g_strdup(saved_errno == ETIMEDOUT ? "hết thời gian chờ kết nối"
                                                     : g_strerror(saved_errno));
        return FALSE;
    }

    struct timeval snd = { .tv_sec = SEND_TIMEOUT_SEC, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd) < 0)
        g_printerr("caro_client: warning: SO_SNDTIMEO: %s\n", g_strerror(errno));

    sock_fd = fd;
    frame_init(&rx);
    /* The GIOChannel is only used to watch the fd; we still read it with recv() ourselves.
     * G_IO_HUP / G_IO_ERR also wake us up: the recv() in the callback then returns 0 or -1. */
    channel = g_io_channel_unix_new(fd);
    watch_id = g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, on_socket_ready, NULL);
    connected = TRUE;
    remember_peer(host, port);
    return TRUE;
}

gboolean net_is_connected(void)
{
    return connected;
}

gboolean net_is_connected_to(const char *host, int port)
{
    return connected && port == conn_port && strcmp(host, conn_host) == 0;
}

/* ---- receive ------------------------------------------------------------------------------------ */

static gboolean on_socket_ready(GIOChannel *source, GIOCondition cond, gpointer data)
{
    (void)source;
    (void)cond;
    (void)data;

    if (frame_recv(&rx, sock_fd) <= 0) {      /* 0: server closed; -1: error */
        net_on_lost();
        return G_SOURCE_REMOVE;
    }

    /* Drain every complete frame: one recv() may carry several glued frames. */
    static char msg[FRAME_MAX_PAYLOAD + 1];
    guint gen = generation;
    int k;
    while ((k = frame_next(&rx, msg, sizeof msg)) == 1) {
        dispatch(msg);
        if (gen != generation)                 /* a callback closed the connection (LOGOUT) */
            return G_SOURCE_REMOVE;
    }
    if (k < 0) {                               /* malformed frame = lost connection (§1) */
        g_printerr("caro_client: malformed frame from the server\n");
        net_on_lost();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ---- send --------------------------------------------------------------------------------------- */

static gboolean lost_idle(gpointer data)
{
    (void)data;
    lost_idle_id = 0;
    net_on_lost();
    return G_SOURCE_REMOVE;
}

gboolean net_request(const char *payload, ResponseCb cb, gpointer user_data)
{
    size_t n = strlen(payload);
    if (!connected || n == 0 || n > FRAME_MAX_PAYLOAD)
        return FALSE;

    /* Queue first, then send: the response can only be handled once we are back in the main
     * loop, and by then the callback is already waiting at its place in the FIFO. */
    dispatch_expect(payload, cb, user_data);
    dispatch_trace(">>", payload);

    if (app.fake_mode) {
        fake_handle(payload);
        return TRUE;
    }
    if (frame_send(sock_fd, payload) < 0) {
        /* The connection is dead. Report it from the main loop, not from here, so the caller's
         * callback is never run while the caller is still inside net_request(). */
        g_printerr("caro_client: send failed: %s\n", g_strerror(errno));
        if (lost_idle_id == 0)
            lost_idle_id = g_idle_add(lost_idle, NULL);
    }
    return TRUE;
}

/* ---- close -------------------------------------------------------------------------------------- */

void net_close(void)
{
    if (!connected)
        return;
    connected = FALSE;               /* first: a callback below that calls net_request() gets FALSE */
    generation++;

    if (watch_id != 0) {
        g_source_remove(watch_id);
        watch_id = 0;
    }
    if (channel != NULL) {
        g_io_channel_unref(channel);
        channel = NULL;
    }
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
    if (lost_idle_id != 0) {
        g_source_remove(lost_idle_id);
        lost_idle_id = 0;
    }
    if (app.fake_mode)
        fake_stop();

    dispatch_fail_pending();         /* last: every waiting request learns it will get no answer */
}

void net_on_lost(void)
{
    if (!connected)
        return;
    net_close();
    app_go_login("Mất kết nối tới server");
}
