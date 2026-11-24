/*
 * server/protocol.h — interface of protocol.c: handle one complete request from a client.
 * Owner: Hân.
 */
#ifndef CARO_SERVER_PROTOCOL_H
#define CARO_SERVER_PROTOCOL_H

#include "server.h"

/* Handle one complete request of c (message is modified: the '|' become '\0'). Sends exactly one
 * response. May end c's session or any other session (a failed send): check fd afterwards. */
void handle_message(Client *c, char *msg);

/* A field that must be a decimal integer (docs/protocol.md section 2): an optional '-' then digits
 * only, the whole string, no overflow. 1 = ok (*out set), 0 = not a valid integer. */
int parse_long(const char *s, long *out);

#endif /* CARO_SERVER_PROTOCOL_H */
