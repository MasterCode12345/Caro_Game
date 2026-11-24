/*
 * server/net.c — connection layer: accept, per-client receive, reply(), session_end().
 * Owner: Hân.
 *
 * session_end() is the only way a session ends while the server runs: it drops the pending
 * challenges, takes the client out of its game (DISCONNECT or OPP_LEFT), tells the others
 * OFFLINE and closes the socket. It sets fd = -1 first, so two clients closing together cannot
 * make it call itself again and again (docs/issues.md #001).
 */
#include "net.h"
#include "protocol.h"
#include "log.h"
#include "lobby.h"
#include "match.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

/* close() and log an error: nothing else can be done about it (the fd is gone either way). */
static void close_fd(int fd)
{
    if (close(fd) < 0)
        log_event("close failed: %s", strerror(errno));
}

/* Accept a new client and assign a free slot. */
int accept_client(int listenfd)
{
    struct sockaddr_in peer;
    int fd;

    do {
        socklen_t peer_len = sizeof(peer);
        fd = accept(listenfd, (struct sockaddr *)&peer, &peer_len);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        log_event("accept error: %s", strerror(errno));
        return -1;
    }

    /* select() cannot watch a descriptor >= FD_SETSIZE: FD_SET() would write outside the
     * fd_set. This is the only place where a descriptor enters clients[], so check it here. */
    if (fd >= FD_SETSIZE) {
        log_event("connection rejected: fd %d >= FD_SETSIZE", fd);
        close_fd(fd);
        return -1;
    }

    /* Nagle's algorithm + the client's delayed ACK would hold small responses for ~40 ms. The
     * game still works without the option, so a failure is only logged. */
    int no_delay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) < 0)
        log_event("TCP_NODELAY failed: %s", strerror(errno));

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == -1) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        log_event("connection rejected: client table full");
        close_fd(fd);
        return -1;
    }

    Client *c = &clients[slot];

    /* Set send timeout to 2 seconds. */
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        log_event("SO_SNDTIMEO failed: %s", strerror(errno));
        close_fd(fd);
        return -1;
    }

#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &no_sigpipe, sizeof(no_sigpipe)) < 0) {
        log_event("SO_NOSIGPIPE failed: %s", strerror(errno));
        close_fd(fd);
        return -1;
    }
#endif

    /* Store client address as IP:port. */
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) == NULL) {
        log_event("inet_ntop failed: %s", strerror(errno));
        close_fd(fd);
        return -1;
    }

    c->fd = fd;

    snprintf(c->addr, sizeof(c->addr), "%s:%u",
             ip, (unsigned int)ntohs(peer.sin_port));

    /* Initialize client slot data. */
    c->state = SESSION_CONNECTED;
    c->user_id = 0;
    c->username[0] = '\0';
    c->elo = 0;
    c->wins = 0;
    c->losses = 0;
    c->draws = 0;
    c->game = NULL;
    c->queued_at = 0;

    frame_init(&c->in);

    log_event("connected: %s", c->addr);
    return 0;
}

/* Send a formatted response: code|cmd[|data]. */
int reply(Client *c, int code, const char *cmd, const char *fmt, ...)
{
    if (c == NULL || c->fd < 0)
        return -1;

    char payload[FRAME_MAX_PAYLOAD + 1];
    int len;

    if (fmt == NULL) {
        len = snprintf(payload, sizeof(payload), "%d|%s", code, cmd);
    } else {
        char data[FRAME_MAX_PAYLOAD + 1];

        va_list ap;
        va_start(ap, fmt);
        int data_len = vsnprintf(data, sizeof(data), fmt, ap);
        va_end(ap);

        if (data_len < 0 || (size_t)data_len >= sizeof(data)) {
            session_end(c);
            return -1;
        }

        len = snprintf(payload, sizeof(payload), "%d|%s|%s",
                       code, cmd, data);
    }

    if (len < 0 || (size_t)len >= sizeof(payload)) {
        session_end(c);
        return -1;
    }

    log_line(c, cmd, code);

    if (frame_send(c->fd, payload) < 0) {
        log_event("send failed: %s", c->addr);
        session_end(c);
        return -1;
    }

    return 0;
}

/* Send a formatted event payload directly. */
int send_event(Client *c, const char *fmt, ...)
{
    if (c == NULL || c->fd < 0 || fmt == NULL)
        return -1;

    char payload[FRAME_MAX_PAYLOAD + 1];

    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);

    if (len < 0 || (size_t)len >= sizeof(payload)) {
        session_end(c);
        return -1;
    }

    if (frame_send(c->fd, payload) < 0) {
        log_event("event send failed: %s", c->addr);
        session_end(c);
        return -1;
    }

    return 0;
}

/* Close a client session safely. */
void session_end(Client *c)
{
    if (c == NULL || c->fd < 0)
        return;

    /* Preserve account information before resetting the client. */
    int was_logged_in = (c->state == SESSION_LOBBY ||
                         c->state == SESSION_IN_QUEUE ||
                         c->state == SESSION_IN_GAME);

    char username[USERNAME_MAX + 1];
    int elo = c->elo;

    snprintf(username, sizeof(username), "%s", c->username);

    /* Mark the slot closed FIRST. Everything below sends events to OTHER clients, and a failed
     * send ends that client's session, whose own broadcast would try to write to c again and
     * call session_end(c) again. With fd = -1 every send_event() to c returns at once and the
     * guard at the top of this function stops the loop (two clients closing together used to
     * recurse hundreds of times). The log line needs c->addr, so addr is cleared last. */
    int fd = c->fd;
    c->fd = -1;

    /* Pending challenges (outgoing or incoming) die with the session; the other side gets
     * CHALLENGE_CANCELLED. They are matched by pointer, so fd = -1 does not matter. */
    challenges_drop_for(c);

    /* Take c out of its game so no Game keeps a pointer to this slot. A running game ends with
     * reason DISCONNECT; after an ended one the opponent gets OPP_LEFT. */
    game_detach(c);

    /* game_detach() may have ended a running game and changed c's ELO: use the new value. */
    elo = c->elo;
    /* Notify other logged-in clients (broadcast_status skips fd < 0, so never c itself). */
    if (was_logged_in && username[0] != '\0') {
        broadcast_status(c, username, PS_OFFLINE, elo);
    }

    log_event("disconnected: %s", c->addr);

    close_fd(fd);

    c->addr[0] = '\0';
    c->state = SESSION_CONNECTED;
    c->user_id = 0;
    c->username[0] = '\0';
    c->elo = 0;
    c->wins = 0;
    c->losses = 0;
    c->draws = 0;
    c->game = NULL;
    c->queued_at = 0;

    frame_init(&c->in);
}

/* Called by the select() loop when c's socket is readable.
 *
 * frame_recv() is called exactly ONCE: select() only promises that one recv() will not block, a
 * second one could stop the whole server until this client sends more. Then every complete
 * frame in the buffer is handled (several requests may arrive glued together); an incomplete
 * one stays in c->in until the next time the socket is readable. */
void net_receive(Client *c)
{
    if (c == NULL || c->fd < 0)
        return;

    /* 0: the peer closed the connection; -1: error. */
    if (frame_recv(&c->in, c->fd) <= 0) {
        session_end(c);
        return;
    }

    char msg[FRAME_MAX_PAYLOAD + 1];

    /* handle_message() can end c's own session (a failed send): check fd after every message. */
    while (c->fd >= 0) {
        int result = frame_next(&c->in, msg, sizeof(msg));

        if (result == 0)
            break;                      /* need more bytes */

        if (result < 0) {
            /* LENGTH 0 or > 16384: after it the stream has no known frame boundary any more,
             * so the only thing left is to close (docs/protocol.md section 1). Logged before
             * session_end(), which clears c->addr. */
            log_event("malformed frame from %s", c->addr);
            session_end(c);
            return;
        }

        handle_message(c, msg);
    }
}
