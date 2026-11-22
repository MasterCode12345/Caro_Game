/*
 * server/lobby.h — interface of lobby.c.
 * Owner: Hân.
 *
 * S1: PLAYER_STATUS broadcast.
 * S2: LIST_PLAYERS and challenges (create / reply / cancel / expiry / cleanup).
 * Later stages: matchmaking.
 */
#ifndef CARO_SERVER_LOBBY_H
#define CARO_SERVER_LOBBY_H

#include "server.h"

/* Broadcast player status to every other logged-in client. */
void broadcast_status(const Client *exclude,
                     const char *username,
                     const char *status,
                     int elo);

/* Status string of a logged-in client as shown to others: IDLE / IN_QUEUE / IN_GAME. */
const char *client_status(const Client *c);

/* Request handlers (signature of the command table in protocol.c). */
void handle_list_players(Client *c, char *fields[], int nfields);
void handle_challenge(Client *c, char *fields[], int nfields);
void handle_challenge_reply(Client *c, char *fields[], int nfields);
void handle_challenge_cancel(Client *c, char *fields[], int nfields);

/* Remove every pending challenge that involves c (outgoing or incoming); the other side gets
 * CHALLENGE_CANCELLED|cid. Call when c enters a game or the queue, logs out or disconnects. */
void challenges_drop_for(Client *c);

/* Called once per select() loop from tick(): expire challenges older than
 * CHALLENGE_EXPIRE_SEC (both sides get CHALLENGE_EXPIRED|cid). */
void lobby_tick(time_t now);

#endif /* CARO_SERVER_LOBBY_H */