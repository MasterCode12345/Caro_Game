/*
 * server/net.h — connection layer.
 * Owner: Hân.
 */
#ifndef CARO_SERVER_NET_H
#define CARO_SERVER_NET_H

#include "server.h"

int accept_client(int listenfd);
int reply(Client *c, int code, const char *cmd, const char *fmt, ...);
int send_event(Client *c, const char *fmt, ...);
void session_end(Client *c);

#endif /* CARO_SERVER_NET_H */