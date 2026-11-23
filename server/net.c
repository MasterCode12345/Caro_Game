/*
 * server/net.c — connection layer: accept, per-client receive, reply(), session_end().
 * Owner: Hân.
 *
 * S0: accept connections, receive framed messages, send replies and close sessions.
 * S1: broadcast OFFLINE when a logged-in client disconnects.
 * S2: drop pending challenges in session_end().
 * S3: take the client out of its game in session_end(); fd is set to -1 first so two clients
 *     closing together cannot make session_end() call itself again and again.
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

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

int one = 1;

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
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) < 0)
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
        close(fd);
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
        close(fd);
        return -1;
    }

#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &no_sigpipe, sizeof(no_sigpipe)) < 0) {
        log_event("SO_NOSIGPIPE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
#endif

    /* Store client address as IP:port. */
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip)) == NULL) {
        log_event("inet_ntop failed: %s", strerror(errno));
        close(fd);
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

    /* S3: take c out of its game so no Game keeps a pointer to this slot (S4 adds the
     * DISCONNECT result). */
    game_detach(c);
    
    /* game_detach() may have ended a running game and changed c's ELO: use the new value. */
    elo = c->elo;
    /* Notify other logged-in clients (broadcast_status skips fd < 0, so never c itself). */
    if (was_logged_in && username[0] != '\0') {
        broadcast_status(c, username, PS_OFFLINE, elo);
    }

    log_event("disconnected: %s", c->addr);

    close(fd);

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

/* Receive and drain complete frames from a client. */
void net_receive(Client *c)
{
    if (c == NULL || c->fd < 0)
        return;

    int n = frame_recv(&c->in, c->fd);

    if (n <= 0) {
        session_end(c);
        return;
    }

    char message[FRAME_MAX_PAYLOAD + 1];

    while (c->fd >= 0) {
        int result = frame_next(&c->in, message, sizeof(message));

        if (result == 0)
            break;

        if (result < 0) {
            log_event("malformed frame: %s", c->addr);
            session_end(c);
            return;
        }

        handle_message(c, message);
    }
}