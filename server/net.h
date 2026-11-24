/*
 * server/net.h — connection layer.
 * Owner: Hân.
 */
#ifndef CARO_SERVER_NET_H
#define CARO_SERVER_NET_H

#include "server.h"

/* Accept one connection into a free slot of clients[]. 0 ok, -1 rejected or error (logged). */
int accept_client(int listenfd);

/* c's socket is readable: one recv(), then handle every complete frame. May end the session. */
void net_receive(Client *c);

/* code|cmd[|data] to the sender of a request. A failed send ends the session: returns -1. */
int reply(Client *c, int code, const char *cmd, const char *fmt, ...);

/* An event (the whole payload) to any client. A failed send ends that session: returns -1. */
int send_event(Client *c, const char *fmt, ...);

/* End a session: cleanup, events to the others, close the socket, fd = -1. Safe to call twice. */
void session_end(Client *c);

#endif /* CARO_SERVER_NET_H */
