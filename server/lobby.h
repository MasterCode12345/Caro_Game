/*
 * server/lobby.h — interface of lobby.c.
 * Owner: Hân.
 *
 * S1: PLAYER_STATUS broadcast.
 * Later stages: players list, challenges and matchmaking.
 */
#ifndef CARO_SERVER_LOBBY_H
#define CARO_SERVER_LOBBY_H

#include "server.h"

/* Broadcast player status to every other logged-in client. */
void broadcast_status(const Client *exclude,
                     const char *username,
                     const char *status,
                     int elo);

#endif /* CARO_SERVER_LOBBY_H */