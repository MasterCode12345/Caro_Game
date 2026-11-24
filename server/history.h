/*
 * server/history.h — interface of history.c.
 * Owner: Hân.
 *
 * HISTORY, REPLAY, LEADERBOARD (docs/protocol.md section 6.4). STATS is in lobby.c.
 */
#ifndef CARO_SERVER_HISTORY_H
#define CARO_SERVER_HISTORY_H

#include "server.h"

/* Request handlers (signature of the command table in protocol.c). */
void handle_history(Client *c, char *fields[], int nfields);
void handle_replay(Client *c, char *fields[], int nfields);
void handle_leaderboard(Client *c, char *fields[], int nfields);

#endif /* CARO_SERVER_HISTORY_H */
