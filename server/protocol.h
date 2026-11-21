/*
 * server/protocol.h — interface of protocol.c: handle one complete request from a client.
 * Owner: Hân.
 *
 * S0: handle one complete message received from a client.
 */
#ifndef CARO_SERVER_PROTOCOL_H
#define CARO_SERVER_PROTOCOL_H

#include "server.h"

void handle_message(Client *client, char *message);

#endif /* CARO_SERVER_PROTOCOL_H */